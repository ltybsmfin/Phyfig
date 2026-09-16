"""formula_writing_plan 的单元测试与真实 PDF 集成测试。

* 结构判定（分数线、局部归属、严格单向嵌套、主体优先）用合成原子，只依赖标准库；
* 字形归属用假 text page/page object 验证"按对象标识而非包围盒"；
* 像素归属用合成掩码验证"最小包含框优先"（大根号框不会吞掉被开方字形）；
* 真实渲染测试在缺少 pdflatex 或 Pillow/pypdfium2 时自动跳过；
* 集成测试直接跑 ``render_formula``（运行时公式走的同一条路径），按计划里的
  文字/规则线顺序验证书写顺序，而不是只验证"每个连通域都出现了"。
"""
import ctypes
import importlib.util
import re
import shutil
import types
import unittest

from formula_writing_plan import (GLYPH, RULE, WritingPlanError, bind_components, extract_atoms,
                                  ink_components, mark_fraction_bars, order_atoms)
from render_formula import render_formula

HAVE_PDFLATEX = shutil.which('pdflatex') is not None
HAVE_DEPS = (importlib.util.find_spec('PIL') is not None
             and importlib.util.find_spec('pypdfium2') is not None)
NEEDS_RENDER = unittest.skipUnless(HAVE_PDFLATEX and HAVE_DEPS, '需要 pdflatex 与 Pillow/pypdfium2')


def glyph(x0, y0, x1, y1, text='x'):
    return {'kind': GLYPH, 'box': [float(x0), float(y0), float(x1), float(y1)],
            'text': text, 'code': ord(text) if text else -1}


def rule(x0, y0, x1, y1):
    return {'kind': RULE, 'box': [float(x0), float(y0), float(x1), float(y1)], 'text': '', 'code': -1}


def ordered(atoms):
    return order_atoms(mark_fraction_bars(atoms))


def labels_of(atoms):
    return [atom['text'] if atom['kind'] == GLYPH else '[bar]' if atom.get('bar') else '[line]'
            for atom in atoms]


class MaskBuilder:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.alpha = bytearray(width * height)

    def rect(self, x0, y0, x1, y1, value=255):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.alpha[y * self.width + x] = value

    def view(self):
        return bytes(self.alpha)


class ComponentTest(unittest.TestCase):
    def test_raster_order_and_four_connectivity(self):
        mask = MaskBuilder(12, 12)
        mask.rect(5, 5, 5, 5)
        mask.rect(6, 6, 6, 6)          # only corner-touching: two regions
        mask.rect(1, 1, 3, 3)
        count, labels = ink_components(mask.view(), mask.width, mask.height)
        self.assertEqual(count, 3)
        # The seed scan is row major, so the top-left block is component 0.
        self.assertEqual(labels[1 * mask.width + 1], 0)
        self.assertNotEqual(labels[5 * mask.width + 5], labels[6 * mask.width + 6])

    def test_coverage_below_ink_level_is_not_a_component(self):
        mask = MaskBuilder(4, 4)
        mask.rect(1, 1, 2, 2, value=127)
        count, _ = ink_components(mask.view(), mask.width, mask.height)
        self.assertEqual(count, 0)


class StructureTest(unittest.TestCase):
    def test_fraction_bar_is_hoisted_before_numerator_and_denominator(self):
        # Content order is what pdfTeX writes: numerator, bar, denominator.
        numerator = glyph(20, 12, 30, 20, 'n')
        bar = rule(18, 9, 40, 10)
        denominator = glyph(20, 1, 26, 6, 'd')
        atoms = [glyph(0, 0, 5, 8, 'T'), glyph(7, 3, 12, 5, '='), numerator, bar, denominator]
        self.assertEqual(labels_of(ordered(atoms)), ['T', '=', '[bar]', 'n', 'd'])

    def test_nested_fraction_recurses_bar_first(self):
        inner = rule(20, 16, 30, 17)
        outer = rule(18, 9, 40, 10)
        a, b, c = glyph(21, 19, 25, 24, 'a'), glyph(21, 11, 25, 15, 'b'), glyph(20, 1, 26, 6, 'c')
        atoms = [a, inner, b, outer, c]
        # outer bar, then its numerator (a nested fraction: bar, a, b), then c.
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', '[bar]', 'a', 'b', 'c'])

    def test_nested_fraction_with_equal_spans_recurses_bar_first(self):
        # 内外两条分数线 x 跨度完全相同：只能靠"嵌套间隙必须比最近间隙紧 2 倍"定方向。
        atoms = [glyph(0, 40, 4, 48, 'a'), rule(0, 34, 4, 34.4), glyph(0, 24, 4, 32, 'b'),
                 rule(0, 20, 4, 20.4), glyph(0, 10, 4, 18, 'c')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', '[bar]', 'a', 'b', 'c'])

    def test_fraction_inside_a_denominator(self):
        # 外侧分数线覆盖整个分式（宽），内层分式是它的分母；b 到两条分数线几乎等距
        # （TeX 间距），必须归更深的内层，否则外侧会把内层的分子抢走。
        outer, inner = rule(0, 30, 10, 30.4), rule(3, 22, 9, 22.4)
        atoms = [glyph(1, 34, 4, 38, 'a'), outer, glyph(4, 24, 8, 28, 'b'),
                 inner, glyph(4, 13, 8, 17, 'c')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', 'a', '[bar]', 'b', 'c'])

    def test_aligned_rows_stay_independent_fractions(self):
        # 复现回归：aligned 里上下两行的独立分式 x 跨度相同，曾经被互认成嵌套
        # （根集合为空 → 顺序退回内容流 a bar1 b c bar2 d）。
        atoms = [glyph(1, 34, 3, 38, 'a'), rule(0, 30, 4, 30.4), glyph(1, 24, 3, 28, 'b'),
                 glyph(1, 14, 3, 18, 'c'), rule(0, 10, 4, 10.4), glyph(1, 4, 3, 8, 'd')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', 'a', 'b', '[bar]', 'c', 'd'])

    def test_sibling_fractions_stay_left_to_right(self):
        bar1, bar2 = rule(0, 9, 10, 10), rule(20, 9, 30, 10)
        atoms = [glyph(2, 12, 8, 16, 'a'), bar1, glyph(2, 2, 8, 6, 'b'), glyph(12, 6, 16, 10, '+'),
                 glyph(22, 12, 28, 16, 'c'), bar2, glyph(22, 2, 28, 6, 'd')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', 'a', 'b', '+', '[bar]', 'c', 'd'])

    def test_rule_decoration_follows_its_operand_side(self):
        # 分子里的上横线跟着分子走（分数线 → 分子横线 → 分子字形 → 分母）。
        atoms = [rule(0, 28, 4, 28.4), glyph(0, 22, 4, 26, 'a'), rule(0, 16, 4, 16.4),
                 glyph(0, 8, 4, 12, 'b')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', '[line]', 'a', 'b'])

    def test_overline_is_not_a_fraction_bar(self):
        overline = rule(17, 9, 39, 10)
        atoms = [glyph(0, 0, 4, 20, 'r'), overline, glyph(5, 2, 12, 8, 'a'), glyph(13, 2, 18, 8, 'b')]
        # Radical, its overbar and the radicand stay contiguous in content order.
        self.assertEqual(labels_of(ordered(atoms)), ['r', '[line]', 'a', 'b'])

    def test_thick_rule_is_not_a_fraction_bar(self):
        block = rule(10, 8, 20, 14)
        atoms = [glyph(12, 16, 16, 22, 'a'), block, glyph(12, 2, 16, 6, 'b')]
        self.assertEqual(labels_of(ordered(atoms)), ['a', '[line]', 'b'])

    def test_adjacent_rows_are_not_claimed_across_the_band(self):
        # 只有一条分数线时，下一行的字形不能因为 x 相交就被当成它的分母。
        atoms = [rule(0, 30, 10, 30.4), glyph(1, 34, 4, 38, 'a'), glyph(1, 24, 4, 28, 'b'),
                 glyph(1, 4, 4, 8, 'x')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', 'a', 'b', 'x'])


class StackTest(unittest.TestCase):
    """主体优先：算子 / 底数先出，上下限与箭头、帽子等装饰随后。"""

    def test_operator_limits_shipped_first_still_follow_the_operator(self):
        # 内容流里上限先于算子（真实现象）：operator, upper, lower。
        operator = glyph(0, 16, 16, 32, 'S')
        upper = glyph(6, 36, 10, 40, 'n')
        lower = glyph(2, 8, 5, 12, 'i')
        atoms = [upper, operator, lower]
        self.assertEqual(labels_of(ordered(atoms)), ['S', 'n', 'i'])

    def test_accent_shipped_first_follows_its_base(self):
        accent = glyph(1, 14, 6, 16, '\u20d7')
        base = glyph(0, 8, 5, 13, 'v')
        atoms = [accent, base, glyph(6, 6, 9, 11, '0')]
        self.assertEqual(labels_of(ordered(atoms)), ['v', '\u20d7', '0'])

    def test_wide_decoration_over_a_row_comes_after_the_row(self):
        # 一行两个字形上的宽箭头：先 A、B，再箭头。
        arrow = glyph(0, 20, 10, 22, '\u2192')
        atoms = [arrow, glyph(1, 12, 4, 18, 'A'), glyph(5, 12, 8, 18, 'B')]
        self.assertEqual(labels_of(ordered(atoms)), ['A', 'B', '\u2192'])

    def test_rows_apart_are_not_stacked(self):
        atoms = [glyph(0, 40, 4, 48, 'a'), glyph(0, 10, 4, 18, 'd')]
        self.assertEqual(labels_of(ordered(atoms)), ['a', 'd'])

    def test_fraction_line_blocks_stacking(self):
        # 分子/分母被分数线隔开，永远不是"主体 + 上下标"。
        atoms = [glyph(0, 22, 4, 26, 'a'), rule(0, 16, 4, 16.4), glyph(0, 8, 4, 12, 'b')]
        self.assertEqual(labels_of(ordered(atoms)), ['[bar]', 'a', 'b'])


class BindingTest(unittest.TestCase):
    def test_smallest_containing_box_wins(self):
        # The radical's tight box spans the radicand's column, but the radicand's
        # own box is smaller: the radicand must never be swallowed by it.
        mask = MaskBuilder(24, 12)
        mask.rect(2, 2, 5, 5)
        mask.rect(14, 2, 17, 5)
        boxes = [[0., 0., 24., 12.], [13., 1., 18., 6.]]
        owners, count = bind_components([glyph(0, 0, 1, 1), glyph(0, 0, 1, 1)], boxes,
                                        mask.view(), mask.width, mask.height)
        self.assertEqual(count, 2)
        self.assertEqual(owners[0], [0])
        self.assertEqual(owners[1], [1])

    def test_component_without_any_box_is_rejected(self):
        mask = MaskBuilder(10, 10)
        mask.rect(1, 1, 3, 3)
        with self.assertRaises(WritingPlanError):
            bind_components([glyph(0, 0, 1, 1)], [[20., 20., 30., 30.]], mask.view(), 10, 10)

    def test_merged_radical_and_overbar_stay_one_unit(self):
        # In the raster the radical sign and its overbar really are one region;
        # whichever of the two boxes wins, it is still one unit and the rest of
        # the formula keeps its own components.
        mask = MaskBuilder(40, 12)
        mask.rect(1, 1, 8, 10)     # radical + overbar blob
        mask.rect(12, 2, 15, 5)    # radicand glyph
        boxes = [[1., 1., 9., 11.], [1., 9., 30., 11.], [11., 1., 16., 6.]]
        owners, count = bind_components([glyph(0, 0, 1, 1), rule(0, 0, 1, 1), glyph(0, 0, 1, 1)],
                                        boxes, mask.view(), mask.width, mask.height)
        self.assertEqual(count, 2)
        self.assertEqual(len(owners[0]) + len(owners[1]), 1)
        self.assertEqual(owners[2], [1])


_PDFIUM = types.SimpleNamespace(raw=types.SimpleNamespace(FPDF_PAGEOBJ_TEXT=1, FPDF_PAGEOBJ_PATH=2))
_TEXT_TYPE = _PDFIUM.raw.FPDF_PAGEOBJ_TEXT
_PATH_TYPE = _PDFIUM.raw.FPDF_PAGEOBJ_PATH


class _FakeChar:
    def __init__(self, char, box, owner):
        self.char, self.box, self.owner = char, box, owner


class _FakeTextPage:
    def __init__(self, chars):
        self.chars = chars
        self.closed = False

    def count_chars(self):
        return len(self.chars)

    def get_text_range(self, index, count=1):
        return self.chars[index].char[:count]

    def get_charbox(self, index, loose=False):
        return list(self.chars[index].box)

    def get_textobj(self, index):
        owner = self.chars[index].owner
        return None if owner is None else types.SimpleNamespace(raw=ctypes.c_void_p(owner))

    def close(self):
        self.closed = True


class _FakeObject:
    def __init__(self, kind, box, address):
        self.type, self.box, self.raw = kind, box, ctypes.c_void_p(address)

    def get_bounds(self):
        return list(self.box)


class _FakePage:
    def __init__(self, chars, objects):
        self.textpage, self.objects = _FakeTextPage(chars), objects

    def get_textpage(self):
        return self.textpage

    def get_objects(self):
        return self.objects


class ExtractionTest(unittest.TestCase):
    """字形必须按对象标识归属：包围盒重叠时也不能抢错。"""

    def test_overlapping_object_bounds_still_map_each_glyph_to_its_own_object(self):
        # 两个文本对象的包围盒完全相同，"A" 的字形框也落在两个框里；只有对象
        # 地址能决定归属（对象顺序 = 内容流顺序），按包围盒包含一定会抢错。
        chars = [_FakeChar('A', [0, 0, 10, 10], 0x1111), _FakeChar('B', [2, 2, 8, 8], 0x2222)]
        objects = [_FakeObject(_TEXT_TYPE, [0, 0, 12, 12], 0x2222),
                   _FakeObject(_TEXT_TYPE, [0, 0, 12, 12], 0x1111)]
        atoms = extract_atoms(_PDFIUM, _FakePage(chars, objects))
        self.assertEqual([atom['text'] for atom in atoms], ['B', 'A'])

    def test_rule_objects_are_kept_in_content_order(self):
        chars = [_FakeChar('a', [0, 0, 4, 4], 0x10), _FakeChar('b', [0, 8, 4, 12], 0x20)]
        objects = [_FakeObject(_TEXT_TYPE, [0, 0, 4, 4], 0x10),
                   _FakeObject(_PATH_TYPE, [0, 5, 4, 5.4], 0x30),
                   _FakeObject(_TEXT_TYPE, [0, 8, 4, 12], 0x20)]
        atoms = extract_atoms(_PDFIUM, _FakePage(chars, objects))
        self.assertEqual([atom['kind'] for atom in atoms], [GLYPH, RULE, GLYPH])

    def test_synthetic_blanks_are_filtered(self):
        chars = [_FakeChar(' ', [0, 0, 0, 0], None), _FakeChar('\r', [0, 0, 0, 0], None),
                 _FakeChar('\n', [0, 0, 0, 0], None), _FakeChar('x', [0, 0, 4, 4], 0x42)]
        objects = [_FakeObject(_TEXT_TYPE, [0, 0, 4, 4], 0x42)]
        atoms = extract_atoms(_PDFIUM, _FakePage(chars, objects))
        self.assertEqual([atom['text'] for atom in atoms], ['x'])

    def test_glyph_without_a_matching_object_is_rejected(self):
        chars = [_FakeChar('x', [0, 0, 4, 4], 0x99)]
        objects = [_FakeObject(_TEXT_TYPE, [0, 0, 4, 4], 0x42)]
        with self.assertRaises(WritingPlanError):
            extract_atoms(_PDFIUM, _FakePage(chars, objects))


def fraction_bar_units(plan):
    """按生成器同样的判据重新识别分数线，用来区分分数线与其它规则线。"""
    boxes = [unit['box'] for unit in plan['units']]
    bars = set()
    for index, unit in enumerate(plan['units']):
        if unit['kind'] != 'rule':
            continue
        box = boxes[index]

        def inside(other):
            return other[0] >= box[0] - 2 and other[2] <= box[2] + 2

        above = any(inside(other) and other[3] <= box[1] + 0.5
                    for other_index, other in enumerate(boxes) if other_index != index)
        below = any(inside(other) and other[1] >= box[3] - 0.5
                    for other_index, other in enumerate(boxes) if other_index != index)
        if above and below:
            bars.add(index)
    return bars


def plan_tokens(plan):
    bars = fraction_bar_units(plan)
    tokens = []
    for index, unit in enumerate(plan['units']):
        if unit['kind'] == 'rule':
            tokens.append('[bar]' if index in bars else '[line]')
        else:
            tokens.append(unit['text'] or 'U+%04X' % unit['code'])
    return tokens


def assert_bar_before_its_operands(test, plan):
    """分数线必须排在它跨度内**最近**的上方/下方单元之前。

    只要求"最近的分子/分母"由几何直接给出，不引入归属模型（否则就是复述实现）；
    更完整的顺序由各用例的 token 断言覆盖。返回检查到的分数线条数。
    """
    boxes = [unit['box'] for unit in plan['units']]
    bars = fraction_bar_units(plan)
    checked = 0
    for bar in sorted(bars):
        box = boxes[bar]
        nearest = {}
        for index, other in enumerate(boxes):
            if index == bar or index in bars:
                continue
            if other[0] < box[0] - 2 or other[2] > box[2] + 2:
                continue
            if other[3] <= box[1] + 0.5:
                gap, side = box[1] - other[3], 'above'
            elif other[1] >= box[3] - 0.5:
                gap, side = other[1] - box[3], 'below'
            else:
                continue
            if side not in nearest or gap < nearest[side][0]:
                nearest[side] = (gap, index)
        for side, (_, index) in nearest.items():
            test.assertLess(bar, index, f'分数线 {bar} 必须排在{side}侧最近的单元 {index} 之前')
        if len(nearest) == 2:
            checked += 1
    return checked


def assert_subject_before_its_marks(test, plan):
    """字形主体必须排在它上方/下方的上下标或装饰之前（由计划盒子独立推导）。

    规则线不参与：分数线与被它分开的上下墨迹是"分数线在前"，不是主体/装饰关系。
    """
    boxes = [unit['box'] for unit in plan['units']]
    kinds = [unit['kind'] for unit in plan['units']]
    checked = 0
    for index, box in enumerate(boxes):
        if kinds[index] == 'rule':
            continue
        for other_index, other in enumerate(boxes):
            if other_index == index or kinds[other_index] == 'rule':
                continue
            if other[3] <= box[1] + 0.5:
                upper, lower = box, other
            elif other[1] >= box[3] - 0.5:
                upper, lower = other, box
            else:
                continue
            gap = upper[1] - lower[3]
            overlap = min(upper[2], lower[2]) - max(upper[0], lower[0])
            if overlap < 0.5 * min(upper[2] - upper[0], lower[2] - lower[0]):
                continue
            if gap > 0.5 * max(upper[3] - upper[1], lower[3] - lower[1]):
                continue
            subject = index if (box[3] - box[1]) >= (other[3] - other[1]) else other_index
            mark = other_index if subject == index else index
            test.assertLess(subject, mark, f'主体单元 {subject} 必须排在装饰/上下标单元 {mark} 之前')
            checked += 1
    return checked


@NEEDS_RENDER
class RenderIntegrationTest(unittest.TestCase):
    def plan_of(self, source):
        result = render_formula(source)
        self.assertTrue(result['ok'])
        self.assertIn('writingPlan', result, '同源 PDF 应给出书写计划')
        return result['writingPlan']

    def test_flight_starts_with_T_then_equals_then_the_fraction_bar(self):
        plan = self.plan_of(r'T=\frac{v_{y0}+\sqrt{v_{y0}^{2}+2gh_0}}{g}')
        self.assertEqual(plan['version'], 1)
        tokens = plan_tokens(plan)
        # Outer row is left to right: T first, then `=`, then the fraction.
        self.assertEqual(tokens[:3], ['T', '=', '[bar]'])
        self.assertEqual(tokens[-1], 'g', '分母 g 必须排在最后')
        # 紧页面渲染下根号字形与它的上横线属于同一块墨迹，上横线没有独立单元；
        # 根号字形本身的 text 是字体编码产物，因此只断言它周围的结构。
        expected = ['v', 'y', '0', '+', None, 'v', '2', 'y', '0', '+', '2', 'g', 'h', '0']
        self.assertEqual(len(tokens[3:-1]), len(expected))
        for got, want in zip(tokens[3:-1], expected):
            if want is not None:
                self.assertEqual(got, want)
        self.assertGreater(assert_bar_before_its_operands(self, plan), 0, '至少验证到一条分数线')

    def test_fraction_bar_precedes_operands_for_a_simple_fraction(self):
        plan = self.plan_of(r'\frac{1}{2}mv^2')
        self.assertEqual(plan_tokens(plan), ['[bar]', '1', '2', 'm', 'v', '2'])
        self.assertEqual(assert_bar_before_its_operands(self, plan), 1)

    def test_nested_fractions_recurse_bar_first(self):
        plan = self.plan_of(r'\frac{\frac{a}{b}}{c}')
        self.assertEqual(plan_tokens(plan), ['[bar]', '[bar]', 'a', 'b', 'c'])
        self.assertEqual(assert_bar_before_its_operands(self, plan), 2)
        plan = self.plan_of(r'\frac{a}{\frac{b}{c}}')
        self.assertEqual(plan_tokens(plan), ['[bar]', 'a', '[bar]', 'b', 'c'])
        plan = self.plan_of(r'\frac{\frac{a}{b}}{\frac{c}{d}}')
        self.assertEqual(plan_tokens(plan), ['[bar]', '[bar]', 'a', 'b', '[bar]', 'c', 'd'])

    def test_aligned_rows_are_independent_fractions(self):
        plan = self.plan_of(r'\begin{aligned}a&=\frac{b}{c}\\d&=\frac{e}{f}\end{aligned}')
        self.assertEqual(plan_tokens(plan),
                         ['a', '=', '[bar]', 'b', 'c', 'd', '=', '[bar]', 'e', 'f'])
        self.assertEqual(assert_bar_before_its_operands(self, plan), 2)

    def test_fraction_under_and_inside_an_overline(self):
        plan = self.plan_of(r'\overline{\frac{a}{b}}')
        self.assertEqual(plan_tokens(plan), ['[bar]', '[line]', 'a', 'b'])
        plan = self.plan_of(r'\frac{\overline{a}}{b}')
        self.assertEqual(plan_tokens(plan), ['[bar]', '[line]', 'a', 'b'])
        self.assertEqual(assert_bar_before_its_operands(self, plan), 1)

    def test_big_operator_keeps_its_limits_after_it(self):
        plan = self.plan_of(r'\sum_{i=1}^{n}i^2')
        tokens = plan_tokens(plan)
        areas = [(unit['box'][2] - unit['box'][0]) * (unit['box'][3] - unit['box'][1])
                 for unit in plan['units']]
        self.assertEqual(areas.index(max(areas)), 0, '求和算子必须排在它的上下限之前')
        # 上下限保持内容流顺序；算子字形本身的编码是字体产物，不做字面断言。
        self.assertEqual(tokens[1:5], ['n', 'i', '=', '1'])
        self.assertGreater(assert_subject_before_its_marks(self, plan), 0, '至少要验证一对主体/上下限')

    def test_integral_sign_precedes_its_limits(self):
        plan = self.plan_of(r'\int_0^T v\,\mathrm{d}t')
        tokens = plan_tokens(plan)
        areas = [(unit['box'][2] - unit['box'][0]) * (unit['box'][3] - unit['box'][1])
                 for unit in plan['units']]
        self.assertEqual(areas.index(max(areas)), 0, '积分号必须排在它的上下限之前')
        self.assertEqual(tokens[1:3], ['T', '0'])

    def test_accent_follows_its_base(self):
        plan = self.plan_of(r'\vec{v}_0')
        tokens = plan_tokens(plan)
        self.assertEqual(tokens[0], 'v', '底数必须先出')
        self.assertEqual(tokens[2], '0')
        self.assertNotIn(tokens[1], ('v', '0'), '第二个单元是矢量箭头')
        self.assertGreater(assert_subject_before_its_marks(self, plan), 0)
        plan = self.plan_of(r'\hat{x}+y')
        self.assertEqual(plan_tokens(plan)[0], 'x', '帽子必须跟在底数之后')
        self.assertGreater(assert_subject_before_its_marks(self, plan), 0)

    def test_radical_stays_ahead_of_its_radicand_fraction(self):
        plan = self.plan_of(r'\sqrt{\frac{a}{b}}')
        tokens = plan_tokens(plan)
        # 根号字形先出（其 text 是字体编码产物，不做字面断言）；紧页面下根号与
        # 它的上横线是同一块墨迹，所以上横线没有独立单元，接着就是被开方分式。
        self.assertEqual(tokens[0], 'r')
        self.assertEqual(tokens[1:], ['[bar]', 'a', 'b'])
        self.assertEqual(assert_bar_before_its_operands(self, plan), 1)

    def test_base_is_followed_by_its_scripts(self):
        plan = self.plan_of(r'v_0^2+x_i')
        tokens = plan_tokens(plan)
        self.assertEqual(tokens[0], 'v')
        # 底数后紧跟它的上下标（内部顺序由内容流决定），然后才是后面的运算符。
        self.assertEqual(sorted(tokens[1:3]), ['0', '2'])
        self.assertEqual(tokens[3:], ['+', 'x', 'i'])

    def test_plan_covers_every_component_exactly_once(self):
        plan = self.plan_of(r'\frac{a+b}{c-d}\times\frac{e}{f}')
        seen = []
        for unit in plan['units']:
            self.assertTrue(unit['components'])
            seen.extend(unit['components'])
        self.assertEqual(sorted(seen), list(range(plan['componentCount'])))
        self.assertEqual(assert_bar_before_its_operands(self, plan), 2, '两个分式的横线都在各自分子分母之前')


def parse_plan_table(root):
    """解析生成的 formula_writing_plan.hpp：name -> (width,height,componentCount,units)。"""
    header = (root / 'assets/formulas/formula_writing_plan.hpp').read_text(encoding='ascii')
    components = {name: [int(value) for value in body.split(',') if value.strip()]
                  for name, body in re.findall(r'inline constexpr int (plan_\w+)\[\] = \{(.*?)\};', header, re.S)}
    units = {}
    for name, body in re.findall(r'inline constexpr Unit (units_\w+)\[\] = \{(.*?)\};', header, re.S):
        units[name] = [(int(offset), int(count), int(kind))
                       for offset, count, kind in re.findall(r'\{plan_\w+\+(\d+),(\d+),(\d+)\}', body)]
    table = {}
    for name, width, height, count, unit_count in re.findall(
            r'\{"(\w+)",(\d+),(\d+),(\d+),units_\w+,(\d+)\}', header):
        table[name] = (int(width), int(height), int(count), units[f'units_{name}'], int(unit_count))
    return table, components


def parse_mask_cache(root):
    """解析 formula_cache.hpp：name -> (width,height,alpha bytes)。"""
    header = (root / 'assets/formulas/formula_cache.hpp').read_text(encoding='ascii')
    data = {name: [int(value) for value in body.replace('\n', '').split(',') if value.strip()]
            for name, body in re.findall(r'inline constexpr unsigned char data_(\w+)\[\] = \{(.*?)\};', header, re.S)}
    masks = {}
    for name, width, height in re.findall(r'\{"(\w+)",(\d+),(\d+),data_\w+,sizeof\(data_\w+\)\}', header):
        alpha = bytearray()
        for index in range(0, len(data[name]), 2):
            alpha.extend(bytes([data[name][index + 1]]) * data[name][index])
        masks[name] = (int(width), int(height), bytes(alpha))
    return masks


class BuiltinPlanTableTest(unittest.TestCase):
    """The checked-in plan table must describe the checked-in masks exactly.

    The built-in masks come from the A4 debug page in generate.py while
    ``render_formula`` renders a tight page, so the two renders are *not* byte
    identical (that is pre-existing). The table therefore has to be checked
    against the very mask it ships with, which is also the invariant the C++
    side relies on: same size, same ink components, every component exactly once.
    """

    def test_every_builtin_plan_covers_its_mask_components(self):
        from pathlib import Path
        root = Path(__file__).resolve().parents[1]
        table, components = parse_plan_table(root)
        masks = parse_mask_cache(root)
        self.assertEqual(set(table), set(masks), '计划表与 alpha 缓存必须一一对应')
        self.assertGreater(len(table), 20)
        for name, (width, height, component_count, units, unit_count) in table.items():
            mask_width, mask_height, alpha = masks[name]
            self.assertEqual((width, height), (mask_width, mask_height), f'{name}: 计划尺寸必须等于掩码尺寸')
            self.assertEqual(len(units), unit_count, f'{name}: 单元数不一致')
            count, _ = ink_components(alpha, mask_width, mask_height)
            self.assertEqual(count, component_count, f'{name}: 连通域数量必须匹配')
            seen = []
            offset = 0
            for unit_offset, unit_components, kind in units:
                self.assertEqual(unit_offset, offset, f'{name}: 单元偏移必须连续')
                self.assertIn(kind, (0, 1), f'{name}: 单元类型只能是字形或规则线')
                self.assertGreater(unit_components, 0, f'{name}: 单元不能为空')
                offset += unit_components
                seen.extend(components[f'plan_{name}'][unit_offset:unit_offset + unit_components])
            self.assertEqual(offset, component_count, f'{name}: 单元必须覆盖全部连通域')
            self.assertEqual(sorted(seen), list(range(component_count)),
                             f'{name}: 每个连通域只能出现一次')

    def test_flight_table_starts_with_T_equals_and_the_fraction_bar(self):
        from pathlib import Path
        root = Path(__file__).resolve().parents[1]
        table, _ = parse_plan_table(root)
        _, _, _, units, _ = table['flight']
        # The first unit is the T glyph, the second the two bars of `=`, and the
        # third is a rule: the fraction bar, before numerator and denominator.
        self.assertEqual(units[0][1:], (3, 0), 'flight 的第一个单元应是 T')
        self.assertEqual(units[1][1:], (2, 0), 'flight 的第二个单元应是 =（两条横）')
        self.assertEqual(units[2][1:], (1, 1), 'flight 的第三个单元应是分数线')


if __name__ == '__main__':
    unittest.main()
