"""把一个已编译公式页的 PDF 变成"书写顺序"计划（writingPlan）。

Write 入场动画一次画一个墨迹区域。光栅阅读顺序（先上后下、先左后右）不是公式
的书写顺序：分式必须先从分数线开始，再画分子和分母；主体后面要紧跟上下标；
根号要和它覆盖的被开方部分连续。

本模块从 *和 alpha 掩码同源的那一页 PDF* 推导顺序，因此计划和掩码不会脱节：

* **字形归属按对象标识，不按包围盒**：每个字符的 ``textpage.get_textobj(i).raw``
  地址与 ``page.get_objects()`` 里对象的 ``raw`` 地址一一对应，所以两个对象包围盒
  重叠时也不会抢错字形；没有所属对象的合成空白字符直接过滤。
* 字形取 text page 的 **紧** 字符框（``get_charbox(i, loose=False)``），那是墨迹
  范围，而不是会盖住被开方区域的 advance 框；规则线取 page object 的精确 bounds。
* **主体优先**：上下标/装饰若在内容流里先于主体出现（``\\vec v`` 的箭头、``\\sum``
  的上限），按"谁更高谁是主体、主体先出"重排成 ``主体 → 上下标/装饰``。
* **分式的局部归属与严格单向嵌套**：分数线只认领"紧贴自己"的墨迹（间隙 <= 最近
  间隙 + 1.5 × 中位字形高，且被更近的分数线抢走时归更近者），所以 aligned 里上下
  两行各自的分式互不认领；分式之间的嵌套必须"紧于最近间隙的 2 倍"，这是严格单向
  的结构判据，仅凭 x 包含 + 上下位置不够（那会把 aliged 里同 x 的两行互认成嵌套）。
* 计划只是把已有的墨迹连通域重新排序/分组，不改变任何 texel 归属，所以正常绘制
  和原 alpha 纹理保持不变；缺失或对不上掩码时调用方回退原地阅读顺序。

已知限制（不假装支持）：``\\overbrace``/大括号等"比主体更高的装饰"会被当成主体；
多行 cases/matrix 的大括号、跨越整行的箭头属于同一种情况；``\\frac{}{b}`` 这种空
分子的极端写法不会被识别为分式；完整 TeX 语法解析不在范围内。
"""
import ctypes
import math

# 计划规模上限：内置公式最大的 flight 有 85 个连通域、几十个原子，留足余量。
MAX_UNITS = 512
MAX_COMPONENTS = 8192
# 掩码 texel 容差：抗锯齿、bbox 取整和"最近框"兜底都可能让墨迹越出框这么多。
BOX_TOLERANCE_PX = 2.5
# 判定"上方/下方/横向包含"时的容差，单位 pt（PDF 用户坐标）。
SPAN_TOLERANCE_PT = 0.6
# 规则线作为分数线的高度上限：绝对值与"相对公式字号"两个口径取大者。
RULE_MAX_HEIGHT_PT = 1.5
RULE_MAX_HEIGHT_RATIO = 0.35
# 主体与其上下标/装饰之间的最大间隙（× 较高者的高度）。
STACK_GAP_RATIO = 0.5
# 规则线（上横线/根号上横线）紧贴内容，允许比字形略大的带宽容差（pt）。
RULE_HUG_TOLERANCE_PT = 3.0
# 分式嵌套必须比"到最近墨迹的间隙"紧这么多倍；aligned 的相邻两行达不到。
LOCAL_ATTACH_RATIO = 2.0

GLYPH = 'glyph'
RULE = 'rule'


class WritingPlanError(ValueError):
    """这一页无法给出可靠书写计划；调用方应省略计划并回退到阅读顺序。"""


def _finite_box(box):
    return (box is not None and len(box) == 4
            and all(isinstance(value, (int, float)) and math.isfinite(value) for value in box))


def _empty_box(box):
    return not _finite_box(box) or box[0] >= box[2] or box[1] >= box[3]


def _object_kind(pdfium, obj):
    if obj.type == getattr(pdfium.raw, 'FPDF_PAGEOBJ_TEXT', 1):
        return GLYPH
    if obj.type == getattr(pdfium.raw, 'FPDF_PAGEOBJ_PATH', 2):
        return RULE
    return None


def _address(handle):
    """把 pypdfium2/ctypes 的对象句柄变成可比较的整数地址（0 表示没有）。"""
    if handle is None:
        return 0
    try:
        return ctypes.cast(handle, ctypes.c_void_p).value or 0
    except (TypeError, ValueError):
        return 0


def extract_atoms(pdfium, page):
    """按内容流顺序返回原子列表：每个字形一个（紧墨迹框），每条实心规则线一个。

    形如 ``{'kind': GLYPH|RULE, 'box': [x0,y0,x1,y1], 'text': str, 'code': int}``，
    坐标是 PDF 用户坐标（pt，y 向上）。

    字形与文本对象的关联完全依赖 ``get_textobj(i).raw`` 的对象地址：包围盒重叠时
    也不会误判。没有文本对象（地址为 0）的合成空白字符被过滤；有文本对象却找不到
    对应 page object 的字符视为不可靠，抛 ``WritingPlanError`` 让调用方省略计划。
    """
    textpage = page.get_textpage()
    try:
        glyphs = []
        for index in range(textpage.count_chars()):
            char = textpage.get_text_range(index, 1)
            if not char or char in ('\r', '\n') or char.isspace():
                continue
            try:
                box = textpage.get_charbox(index, loose=False)
            except TypeError:  # 老版本 pypdfium2 没有 loose 参数：宁可不给计划
                raise WritingPlanError('pypdfium2 不支持紧字符框') from None
            if _empty_box(box):
                continue
            owner = None
            try:
                owner = textpage.get_textobj(index)
            except Exception:  # noqa: BLE001 - 拿不到所属对象就当合成字符处理
                owner = None
            address = _address(owner.raw) if owner is not None else 0
            if address == 0:
                continue  # 合成空白：没有所属文本对象
            glyphs.append({'kind': GLYPH, 'box': [float(value) for value in box],
                           'text': char if 32 <= ord(char) < 127 else '',
                           'code': ord(char), 'owner': address})
        if not glyphs:
            raise WritingPlanError('这一页没有字形墨迹')
        atoms = []
        used = set()
        for obj in page.get_objects():
            kind = _object_kind(pdfium, obj)
            if kind is None:
                continue
            bounds = obj.get_bounds()
            if _empty_box(bounds):
                continue
            if kind == GLYPH:
                address = _address(obj.raw)
                picked = [position for position, glyph in enumerate(glyphs)
                          if position not in used and glyph['owner'] == address]
                if not picked:
                    raise WritingPlanError('文本对象与字符无法按对象标识关联')
                for position in picked:
                    used.add(position)
                    glyph = glyphs[position]
                    atoms.append({'kind': glyph['kind'], 'box': glyph['box'],
                                  'text': glyph['text'], 'code': glyph['code']})
            else:
                atoms.append({'kind': RULE, 'box': [float(value) for value in bounds],
                              'text': '', 'code': -1})
        if len(used) != len(glyphs):
            raise WritingPlanError('有字形没有所属文本对象（疑似合成字符）')
        return atoms
    finally:
        textpage.close()


# ---------------------------------------------------------------------------
# 分数线判定
# ---------------------------------------------------------------------------

def _median_glyph_height(atoms):
    heights = sorted(atom['box'][3] - atom['box'][1] for atom in atoms if atom['kind'] == GLYPH)
    if not heights:
        return 0.0
    return heights[len(heights) // 2]


def _x_inside(inner, outer, tolerance=SPAN_TOLERANCE_PT):
    return inner[0] >= outer[0] - tolerance and inner[2] <= outer[2] + tolerance


def _above(atom, bar, tolerance=SPAN_TOLERANCE_PT):
    return atom['box'][1] >= bar['box'][3] - tolerance


def _below(atom, bar, tolerance=SPAN_TOLERANCE_PT):
    return atom['box'][3] <= bar['box'][1] + tolerance


def mark_fraction_bars(atoms):
    """标记分数线：又薄又长、且自己的 x 跨度内上下都有墨迹的水平规则线。"""
    reference = _median_glyph_height(atoms)
    limit = max(RULE_MAX_HEIGHT_PT, RULE_MAX_HEIGHT_RATIO * reference)
    for atom in atoms:
        atom['bar'] = False
        if atom['kind'] != RULE:
            continue
        box = atom['box']
        width, height = box[2] - box[0], box[3] - box[1]
        if width <= 0.5 or height <= 0 or height > limit:
            continue
        above = below = False
        for other in atoms:
            if other is atom or not _x_inside(other['box'], box):
                continue
            above = above or _above(other, atom)
            below = below or _below(other, atom)
        atom['bar'] = above and below
    return atoms


# ---------------------------------------------------------------------------
# 主体优先：把主体与它上方/下方的上下标、装饰合成一个有序节点
# ---------------------------------------------------------------------------

def _overlap_x(first, second):
    return min(first[2], second[2]) - max(first[0], second[0])


def _box_area(box):
    return max(0.0, box[2] - box[0]) * max(0.0, box[3] - box[1])


def _merge_box(box, other):
    box[0] = min(box[0], other[0]); box[1] = min(box[1], other[1])
    box[2] = max(box[2], other[2]); box[3] = max(box[3], other[3])


def _stack_order(first, second):
    """返回 (在上者, 在下者)；两个盒子不构成上下关系时返回 None。"""
    if first[1] >= second[3]:
        return first, second
    if second[1] >= first[3]:
        return second, first
    return None


def _pair_subject(upper, lower):
    """一对堆叠里上方那个是不是主体：更高者为主体，等高等面积时取下方。"""
    height_upper, height_lower = upper[3] - upper[1], lower[3] - lower[1]
    if height_upper != height_lower:
        return height_upper > height_lower
    return _box_area(upper) > _box_area(lower)


def _rule_between(upper, lower, nodes):
    """两个字形之间是否夹着一条规则线（分式线/上横线）：夹着就不是上下标关系。"""
    band_left, band_right = max(upper[0], lower[0]), min(upper[2], lower[2])
    if band_right <= band_left:
        return False
    for node in nodes:
        if node['kind'] != RULE:
            continue
        box = node['box']
        if box[3] > upper[1] + SPAN_TOLERANCE_PT or box[1] < lower[3] - SPAN_TOLERANCE_PT:
            continue
        if box[2] > band_left and box[0] < band_right:
            return True
    return False


def group_stacks(atoms):
    """把主体和它的上下标/装饰合成一个节点，节点内部**主体在前、装饰在后**。

    主体取堆叠里更高的那个字形（上下标和箭头/帽子/横线都比主体矮），同一行两个
    字形不在竖直堆叠关系里；一行上的宽箭头（如 ``\\overrightarrow{AB}``）会把它的
    每个底数都算主体，于是输出 A、B 之后才是箭头。
    """
    nodes = [{'kind': atom['kind'], 'box': list(atom['box']), 'atoms': [atom],
              'index': index, 'bar': bool(atom.get('bar'))} for index, atom in enumerate(atoms)]
    parents = list(range(len(nodes)))
    scripts = set()

    def find(index):
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    def union(first, second):
        a, b = find(first), find(second)
        if a != b:
            parents[max(a, b)] = min(a, b)

    for first in range(len(nodes)):
        if nodes[first]['kind'] != GLYPH:
            continue
        for second in range(first + 1, len(nodes)):
            if nodes[second]['kind'] != GLYPH:
                continue
            pair = _stack_order(nodes[first]['box'], nodes[second]['box'])
            if pair is None:
                continue
            upper, lower = pair
            gap = upper[1] - lower[3]
            if gap > STACK_GAP_RATIO * max(upper[3] - upper[1], lower[3] - lower[1]):
                continue
            if _overlap_x(upper, lower) < 0.5 * min(upper[2] - upper[0], lower[2] - lower[0]):
                continue
            if _rule_between(upper, lower, nodes):
                continue
            union(first, second)
            upper_index = first if upper is nodes[first]['box'] else second
            lower_index = second if upper_index == first else first
            # 更高的一方是主体；另一方（上下标/箭头/帽子/横线）是装饰。
            scripts.add(lower_index if _pair_subject(upper, lower) else upper_index)

    buckets = {}
    for index in range(len(nodes)):
        buckets.setdefault(find(index), []).append(nodes[index])
    grouped = []
    for members in buckets.values():
        if len(members) == 1:
            grouped.append(members[0])
            continue
        subjects = sorted((member for member in members if member['index'] not in scripts),
                          key=lambda member: member['index'])
        decorations = sorted((member for member in members if member['index'] in scripts),
                             key=lambda member: member['index'])
        if not subjects:  # 全是装饰（理论上不会发生）：退回内容流顺序
            subjects, decorations = members, []
        order = subjects + decorations
        box = list(order[0]['box'])
        for member in order[1:]:
            _merge_box(box, member['box'])
        grouped.append({'kind': 'group', 'box': box,
                        'atoms': [member['atoms'][0] for member in order],
                        'index': min(member['index'] for member in members), 'bar': False})
    grouped.sort(key=lambda node: node['index'])
    return grouped


# ---------------------------------------------------------------------------
# 分式：局部归属 + 严格单向嵌套
# ---------------------------------------------------------------------------

def _side_of(box, bar_box, tolerance=SPAN_TOLERANCE_PT):
    """box 相对分数线在严格的上方/下方，还是跨越它（None）。"""
    if box[1] >= bar_box[3] - tolerance:
        return 'above'
    if box[3] <= bar_box[1] + tolerance:
        return 'below'
    return None


def _gap(box, bar_box, side):
    return box[1] - bar_box[3] if side == 'above' else bar_box[1] - box[3]


def _band(nodes, bar, side, include_bars):
    """分数线某一侧的连通带：从最近墨迹出发，沿间隙递增方向扩张到不再相连。

    `include_bars=False` 是**操作数带**（分数线互不为操作数）；`include_bars=True`
    是**结构带**，用来判断"这条分数线这一侧还有没有别的候选"（内层分数线也算候选，
    所以 `\\frac{a}{\\frac{b}{c}}` 的 b 虽然紧贴外侧分数线，外侧仍有内层可选）。
    """
    entries = []
    for node in nodes:
        if node is bar or (node['bar'] and not include_bars):
            continue
        if _side_of(node['box'], bar['box']) != side or _overlap_x(node['box'], bar['box']) <= 0:
            continue
        entries.append((_gap(node['box'], bar['box'], side), node['index'], node))
    entries.sort(key=lambda entry: (entry[0], entry[1]))
    selected = []
    if entries:
        nearest = entries[0][2]['box']
        edge = nearest[3] if side == 'above' else nearest[1]
        for gap, index, node in entries:
            box = node['box']
            tolerance = RULE_HUG_TOLERANCE_PT if node['kind'] == RULE else SPAN_TOLERANCE_PT
            near = box[1] if side == 'above' else box[3]
            if (near > edge + tolerance) if side == 'above' else (near < edge - tolerance):
                break  # 带按间隙递增排列，后面的只会更远
            selected.append((gap, index, node))
            edge = max(edge, box[3]) if side == 'above' else min(edge, box[1])
    return selected


def _attach_maps(nodes, bars):
    """每个分数线两侧的操作数带、结构带与嵌套半径。

    分子/分母是一个框：它从分数线一直延伸到"最近墨迹的远边"，凡是在这个带内、
    同侧且横向相交的墨迹都属于同一个框——抬高的上标、贴着字形上方的 ``\\overline``
    横线因此都跟着自己的框走，而 aligned 下一行的分式落在带外，不会被误领。
    """
    for bar in bars:
        bar['sides'] = {side: _band(nodes, bar, side, False) for side in ('above', 'below')}
        bar['wide'] = {side: _band(nodes, bar, side, True) for side in ('above', 'below')}
        bar['nest_limit'] = {}
        for side in ('above', 'below'):
            entries = [entry for entry in bar['wide'][side] if not entry[2]['bar']]
            bar['nest_limit'][side] = LOCAL_ATTACH_RATIO * entries[0][0] if entries else None
    return bars


def _extent(bar, nodes, owner):
    """分数线这一整个分式框：分数线本体 + 它拥有的墨迹。"""
    box = list(bar['box'])
    for node in nodes:
        if owner.get(id(node)) is bar:
            _merge_box(box, node['box'])
    return box


def _other_gap(bar, side):
    """分数线另一侧最近的墨迹间隙：内层分式两侧都紧，外侧分母侧通常更松。"""
    entries = bar['sides']['below' if side == 'above' else 'above']
    return entries[0][0] if entries else float('inf')


def _parents(bars, nodes, owner):
    """严格单向嵌套：分式必须紧贴父分数线（间隙 <= 2 × 父的最近间隙）。

    仅凭"x 包含 + 在上/在下"会把 aligned 里上下两行的独立分式互认成嵌套（根集合
    变空、顺序退回内容流）。归属正确时，内外两侧的间隙天然不同（外侧要跨过内层的
    分子/分母），互相认作父子的仍用 x 包含关系定向，实在分不清就都当根。
    """
    extents = {id(bar): _extent(bar, nodes, owner) for bar in bars}
    parent = {}
    for child in bars:
        best = None
        for candidate in bars:
            if candidate is child:
                continue
            side = _side_of(extents[id(child)], candidate['box'])
            if side is None or _overlap_x(extents[id(child)], candidate['box']) <= 0:
                continue
            gap = _gap(extents[id(child)], candidate['box'], side)
            limit = candidate['nest_limit'][side]
            if limit is None or gap > limit:
                continue
            if best is None or gap < best[0] - 1e-6:
                best = (gap, candidate, side)
        if best is not None:
            parent[id(child)] = (best[1], best[2])
    for child in [bar for bar in bars if id(bar) in parent]:
        entry = parent.get(id(child))
        if entry is None:  # 上一轮已经把这条关系消解掉了
            continue
        candidate = entry[0]
        if parent.get(id(candidate), (None,))[0] is not child:
            continue
        del parent[id(child)]
        del parent[id(candidate)]
        child_inside = _x_inside(child['box'], candidate['box'])
        candidate_inside = _x_inside(candidate['box'], child['box'])
        if child_inside and not candidate_inside:
            parent[id(child)] = (candidate, _side_of(extents[id(child)], candidate['box']))
        elif candidate_inside and not child_inside:
            parent[id(candidate)] = (child, _side_of(extents[id(candidate)], child['box']))
    return parent


def _depth(bar, parent, cache):
    if id(bar) in cache:
        return cache[id(bar)]
    cache[id(bar)] = 0  # 防御成环
    entry = parent.get(id(bar))
    value = 0 if entry is None else _depth(entry[0], parent, cache) + 1
    cache[id(bar)] = value
    return value


def _own_nodes(nodes, bars, parent):
    """局部归属：同一块墨迹被多条分数线认领时按结构优先级择一。

    `\\frac{a}{\\frac{b}{c}}` 的 b 同时紧贴外侧分数线（分母侧）与内侧分数线（分子
    侧），两个间隙几乎相等。判据依次是：

    1. **某侧只有这一个候选的分数线优先**：内层的分子侧只有 b（外侧那一侧还有内层
       分数线可选），所以 b 属于内层，外侧不会把内层的分子抢走；
    2. 内层（父子关系更深）优先；
    3. 最近间隙、另一侧紧度、x 跨度、内容流位置。
    """
    depth = {id(bar): _depth(bar, parent, {}) for bar in bars}
    claims = {}
    for bar in bars:
        for side in ('above', 'below'):
            for gap, _, node in bar['sides'][side]:
                claims.setdefault(id(node), []).append((gap, bar, side))
    owner = {}
    for key, entries in claims.items():
        def rank(entry):
            gap, bar, side = entry
            only_candidate = 0 if len(bar['wide'][side]) <= 1 else 1
            return (only_candidate, -depth[id(bar)], gap, _other_gap(bar, side),
                    bar['box'][2] - bar['box'][0], bar['index'])
        owner[key] = min(entries, key=rank)[1]
    return owner


def order_nodes(nodes):
    """结构顺序：主体先出，分式先分数线再分子再分母，其余按内容流从左到右。

    归属与嵌套互相依赖（内层是谁决定墨迹归谁，墨迹归谁又决定分式框的范围），
    所以跑两遍：先按最近间隙归属定出嵌套，再按结构优先级重新归属并更新嵌套。
    """
    bars = [node for node in nodes if node['bar']]
    if not bars:
        return nodes
    _attach_maps(nodes, bars)
    owner = _own_nodes(nodes, bars, {})
    parent = _parents(bars, nodes, owner)
    owner = _own_nodes(nodes, bars, parent)
    parent = _parents(bars, nodes, owner)
    roots = [bar for bar in bars if id(bar) not in parent]

    def emit(bar, seen):
        if id(bar) in seen:  # 防御：父子关系若意外成环就停下，由最终自检回退
            return []
        seen.add(id(bar))
        out = [bar]
        for side in ('above', 'below'):
            elements = [node for node in nodes if owner.get(id(node)) is bar
                        and _side_of(node['box'], bar['box']) == side]
            elements += [node for node in bars if id(node) in parent and parent[id(node)][0] is bar
                         and parent[id(node)][1] == side]
            elements.sort(key=lambda node: node['index'])
            for element in elements:
                out.extend(emit(element, seen) if element['bar'] else [element])
        return out

    ordered = []
    root_ids = {id(root) for root in roots}
    seen = set()
    for node in nodes:
        if id(node) in root_ids:
            ordered.extend(emit(node, seen))
        elif id(node) in owner or id(node) in parent:
            continue  # 由所属/父分数线在自己的位置输出
        else:
            ordered.append(node)
    return ordered


def order_atoms(atoms):
    """原子层面的书写顺序；任何自检失败都退回内容流顺序。"""
    atoms = list(atoms)
    if len(atoms) < 2:
        return atoms
    ordered = order_nodes(group_stacks(atoms))
    flattened = [atom for node in ordered for atom in node['atoms']]
    if sorted(map(id, flattened)) != sorted(map(id, atoms)):
        return atoms
    return flattened


# ---------------------------------------------------------------------------
# 掩码连通域与绑定
# ---------------------------------------------------------------------------

def ink_components(alpha, width, height, level=128):
    """与 C++ ``labelInk`` 完全一致：4 连通、覆盖率 >= level、按光栅种子顺序编号。"""
    labels = [-1] * len(alpha)
    count = 0
    for seed in range(len(alpha)):
        if labels[seed] >= 0 or alpha[seed] < level:
            continue
        labels[seed] = count
        stack = [seed]
        while stack:
            current = stack.pop()
            x = current % width
            for offset in (-1, 1, -width, width):
                nxt = current + offset
                if nxt < 0 or nxt >= len(alpha) or labels[nxt] >= 0 or alpha[nxt] < level:
                    continue
                if offset == -1 and x == 0:
                    continue
                if offset == 1 and x == width - 1:
                    continue
                labels[nxt] = count
                stack.append(nxt)
        count += 1
    return count, labels


def _mask_box(box, crop, page_size, image_size):
    """PDF 用户坐标（y 向上）-> 最终掩码 texel 坐标（y 向下，含 1px 边框）。"""
    scale_x = image_size[0] / page_size[0]
    scale_y = image_size[1] / page_size[1]
    return [box[0] * scale_x - crop[0] + 1.0,
            (page_size[1] - box[3]) * scale_y - crop[1] + 1.0,
            box[2] * scale_x - crop[0] + 1.0,
            (page_size[1] - box[1]) * scale_y - crop[1] + 1.0]


def _distance_to_box(box, x, y):
    return max(max(box[0] - x, 0.0), max(x - box[2], 0.0), max(box[1] - y, 0.0), max(y - box[3], 0.0))


def _assign_texel(boxes, x, y):
    """返回包含该 texel 的最小框；没有包含框时返回容差内最近的框。

    第二项说明这个框是"包含"还是"兜底"来的：一个连通域只有在它至少有一个
    texel 真正落在某个框里时才算归属可信，否则说明边框和墨迹已经对不上。
    """
    best = -1
    best_area = 0.0
    for index, box in enumerate(boxes):
        if x < box[0] - 0.5 or x > box[2] + 0.5 or y < box[1] - 0.5 or y > box[3] + 0.5:
            continue
        area = (box[2] - box[0] + 1.0) * (box[3] - box[1] + 1.0)
        if best < 0 or area < best_area:
            best, best_area = index, area
    if best >= 0:
        return best, True
    nearest, nearest_distance = -1, 0.0
    for index, box in enumerate(boxes):
        distance = _distance_to_box(box, x, y)
        if nearest < 0 or distance < nearest_distance:
            nearest, nearest_distance = index, distance
    if nearest >= 0 and nearest_distance <= BOX_TOLERANCE_PX:
        return nearest, False
    return -1, False


def bind_components(atoms, boxes, alpha, width, height):
    """把每个墨迹连通域绑定到原子；返回 (原子序号 -> [连通域编号], 连通域总数)。

    绑定以 texel 投票为准，并优先取包含该 texel 的最小框。一个连通域可能同时
    落在几个框里（例如根号字形和它的上横线在栅格上本来就粘连成一块），这时取
    票数最多的那个；关键是它绝不会因为根号框大就把被开方字形整块吞掉，因为
    被开方字形自己的框更小、更贴合。
    """
    count, labels = ink_components(alpha, width, height)
    if count <= 0:
        raise WritingPlanError('掩码里没有墨迹连通域')
    if len(atoms) > MAX_UNITS or count > MAX_COMPONENTS:
        raise WritingPlanError('公式结构超过计划上限')
    bounds = [[width, height, -1, -1] for _ in range(count)]
    votes = [{} for _ in range(count)]
    enclosing = [set() for _ in range(count)]
    for index, label in enumerate(labels):
        if label < 0:
            continue
        x, y = index % width, index // width
        box = bounds[label]
        box[0], box[1] = min(box[0], x), min(box[1], y)
        box[2], box[3] = max(box[2], x), max(box[3], y)
        atom, contained = _assign_texel(boxes, float(x) + 0.5, float(y) + 0.5)
        if atom < 0:
            raise WritingPlanError('有墨迹 texel 不属于任何字形或规则线')
        votes[label][atom] = votes[label].get(atom, 0) + 1
        if contained:
            enclosing[label].add(atom)
    owners = [[] for _ in range(len(atoms))]
    for label in range(count):
        tally = votes[label]
        if not tally or not enclosing[label]:
            raise WritingPlanError('墨迹连通域没有落在任何字形/规则线框内，归属不可信')
        owner = max(sorted(tally), key=lambda atom: tally[atom])
        if owner not in enclosing[label]:
            # 得票最多的框其实只靠"最近框"兜底拿到票，说明真正包含这块墨迹的
            # 框另有其人：宁可不给计划，也不要排出错误的书写顺序。
            raise WritingPlanError('墨迹连通域的最佳归属来自兜底距离，归属不可信')
        owners[owner].append(label)
    return owners, count


def build_plan(atoms, alpha, width, height, crop, page_size, image_size):
    """由原子和最终掩码构建可持久化的 writingPlan 字典。"""
    ordered = order_atoms(mark_fraction_bars(atoms))
    boxes = [_mask_box(atom['box'], crop, page_size, image_size) for atom in ordered]
    for box in boxes:
        if not all(math.isfinite(value) for value in box):
            raise WritingPlanError('原子框不是有限数')
    owners, count = bind_components(ordered, boxes, alpha, width, height)
    units = []
    for atom, box, components in zip(ordered, boxes, owners):
        if not components:
            continue
        units.append({'kind': atom['kind'],
                      'box': [round(value, 2) for value in box],
                      'text': atom['text'],
                      'code': atom['code'],
                      'components': sorted(components)})
    if not units:
        raise WritingPlanError('计划里没有任何可见单元')
    return {'version': 1, 'width': width, 'height': height,
            'componentCount': count, 'units': units}
