"""把受限的 ASCII 数学 LaTeX 编译成可移植的 alpha 掩码。

安全边界：

* 只接受数学命令白名单（``COMMANDS`` / ``ENVIRONMENTS``），不是任意 TeX；
* 拒绝非 ASCII、控制字符、``^^`` 记号、注释以及 ``$``/``#``/反引号；
* 只支持 MiKTeX 的 pdfTeX：始终传 ``--disable-installer`` 禁止编译期联网装包，
  再配合 ``-no-shell-escape`` 和受限的 ``openin_any``/``openout_any``；
* 用 ``\\shipout`` 直接输出单个 TeX box，并把 ``\\pdfpagewidth``/``\\pdfpageheight``
  设成 box 尺寸（含边距），所以不会退化成默认 A4 的 MediaBox——否则按 240 DPI
  预渲染会把普通公式全部误判成"尺寸超限"。

CLI::

    python render_formula.py --input request.json --output result.json

``request.json``：``{"latex": "<不含外层 $ 的数学 LaTeX>"}``
``result.json`` 成功：``{ok, latex, name, width, height, rle, displayScale, writingPlan?}``
失败：``{ok: false, error: "..."}``

``rle`` 是 ``[run_length, value, ...]`` 的灰度 alpha 行程编码，行优先、无 padding。

``writingPlan`` 是可选的书写顺序元数据（见 ``formula_writing_plan``）：它只是把
掩码里已有的墨迹连通域重新排序/分组（分式先分数线，主体后紧跟上下标），既不改变
alpha，也不改变任何 texel 的归属。推导失败时字段直接省略，渲染结果照常返回。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid

from formula_writing_plan import build_plan, extract_atoms

# 渲染与尺寸上限（像素）
RENDER_DPI = 240
MAX_WIDTH = 2048
MAX_HEIGHT = 1024
MAX_AREA = 1048576

MAX_SOURCE_BYTES = 4096
MAX_REQUEST_BYTES = 16384
# 页面在 box 四周保留的边距（pt）。用于容纳斜体字形向左的伸出量，
# 真正的裁剪交给 alpha 通道的 bounding box，所以边距只需够大、不必精确。
PAGE_MARGIN_PT = 6
COMPILE_TIMEOUT = 45

COMMANDS = set('frac dfrac tfrac sqrt vec overrightarrow overline underline hat bar dot ddot '
    'mathrm mathit mathbf mathcal mathbb text textrm textbf left right big Big bigg Bigg '
    'sin cos tan cot arcsin arccos arctan sinh cosh tanh ln log exp lim max min '
    'alpha beta gamma delta epsilon varepsilon zeta eta theta vartheta iota kappa lambda mu nu '
    'xi pi varpi rho varrho sigma varsigma tau upsilon phi varphi chi psi omega '
    'Gamma Delta Theta Lambda Xi Pi Sigma Upsilon Phi Psi Omega '
    'cdot times div pm mp le leq ge geq neq approx equiv sim propto infty partial nabla '
    'sum prod int iint oint in notin subset supset cup cap emptyset forall exists '
    'to rightarrow leftarrow Rightarrow Leftarrow leftrightarrow Leftrightarrow '
    'quad qquad hspace operatorname substack underset overset begin end '
    'langle rangle lvert rvert lVert rVert vert Vert '.split())
ENVIRONMENTS = {'aligned', 'cases', 'matrix', 'pmatrix', 'bmatrix', 'vmatrix', 'Vmatrix'}
# 控制符号白名单：\, \; \: \! 空格 \{ \} \| \_ \\
CONTROL_SYMBOLS = {',', ';', ':', '!', ' ', '{', '}', '|', '_', '\\'}
FORBIDDEN_CHARS = ('^^', '%', '$', '#', '`')


def _make_temp_dir(prefix, parent=None):
    """创建临时目录并返回路径。

    ``tempfile.mkdtemp`` 固定用 mode 0o700；本机的文件沙箱会拒绝在 0o700 目录里
    再创建文件（WinError 5）。Windows 上 POSIX mode 对 ACL 基本无影响，所以
    Windows 用 0o755，POSIX 保持 0o700。
    """
    base = parent if parent is not None else tempfile.gettempdir()
    mode = 0o755 if os.name == 'nt' else 0o700
    for _ in range(64):
        candidate = os.path.join(base, prefix + uuid.uuid4().hex)
        try:
            os.mkdir(candidate, mode)
        except FileExistsError:
            continue
        return candidate
    raise OSError(f'无法创建临时目录：{base}')


def validate_latex(source):
    """校验并返回受限数学 LaTeX；任何越界/注入都抛 ValueError。"""
    if not isinstance(source, str) or not source.strip():
        raise ValueError('公式不能为空')
    if len(source.encode('utf-8')) > MAX_SOURCE_BYTES:
        raise ValueError('公式最长 4096 字节')
    if any(ord(c) < 32 or ord(c) > 126 for c in source):
        raise ValueError('只支持 ASCII 数学 LaTeX')
    if any(token in source for token in FORBIDDEN_CHARS):
        raise ValueError('请输入不带外层 $ 的 ASCII 数学 LaTeX；不支持注释或 TeX 转义代码')
    depth = 0
    index = 0
    length = len(source)
    while index < length:
        char = source[index]
        if char == '\\':
            match = re.match(r'\\([a-zA-Z]+|[^a-zA-Z])', source[index:])
            if not match:
                raise ValueError('公式末尾不能是反斜杠')
            command = match[1]
            if command not in COMMANDS and command not in CONTROL_SYMBOLS:
                raise ValueError('不支持的数学命令：\\' + command)
            if command in ('begin', 'end'):
                env = re.match(r'\{([a-zA-Z]+)\}', source[index + len(match[0]):])
                if not env or env[1] not in ENVIRONMENTS:
                    raise ValueError('不支持的数学环境')
            index += len(match[0])
            continue
        if char == '{':
            depth += 1
            if depth > 32:
                raise ValueError('公式嵌套超过 32 层')
        elif char == '}':
            depth -= 1
            if depth < 0:
                raise ValueError('公式花括号不匹配')
        index += 1
    if depth:
        raise ValueError('公式花括号不匹配')
    return source


def build_tex(source):
    """生成只输出一个 hbox、且页面尺寸等于该 box 的完整 .tex 源码。"""
    validate_latex(source)
    return (
        '\\documentclass[12pt]{article}\n'
        '\\usepackage{amsmath,amssymb}\n'
        '\\pagestyle{empty}\n'
        '\\newbox\\fmbox\n'
        '\\newdimen\\fmdim\n'
        '\\begin{document}\n'
        '\\setbox\\fmbox=\\hbox{$\\displaystyle ' + source + '$}\n'
        '\\fmdim=' + str(PAGE_MARGIN_PT) + 'pt\n'
        '\\paperwidth=\\dimexpr\\wd\\fmbox+2\\fmdim\\relax\n'
        '\\paperheight=\\dimexpr\\ht\\fmbox+\\dp\\fmbox+2\\fmdim\\relax\n'
        '\\pdfpagewidth=\\paperwidth\n'
        '\\pdfpageheight=\\paperheight\n'
        '\\pdfhorigin=\\fmdim\n'
        # 实测：\shipout\box 把 box 的左上角放在 (horigin, vorigin)（相对页面
        # 左上角），与 box 的 ht/dp 无关。因此 vorigin 只需等于边距；写成
        # \ht+\fmdim 会把内容整体下移一个 box 高度，裁掉分母/下标。
        '\\pdfvorigin=\\fmdim\n'
        '\\shipout\\box\\fmbox\n'
        '\\end{document}\n'
    )


def _load_render_deps():
    try:
        from PIL import Image
        import pypdfium2 as pdfium
    except ImportError as error:
        raise ValueError('所选 Python 需要 Pillow 和 pypdfium2；可在公式面板选择已装依赖的 Python') from error
    return Image, pdfium


def _decode_log(data):
    if data is None:
        return ''
    if isinstance(data, bytes):
        return data.decode('utf-8', errors='replace')
    return str(data)


def _option_error(log):
    lowered = log.lower()
    return ('unrecognized option' in lowered
            or 'unknown option' in lowered
            or 'invalid option' in lowered)


def _compile_tex(latex, work, run):
    """运行 MiKTeX pdfTeX，返回编译日志；失败抛 ValueError。

    只支持 MiKTeX：始终传 ``--disable-installer``，禁止 TeX 在编译期自动安装
    宏包（也就不存在"去掉参数重试"的退路）。
    """
    env = dict(os.environ)
    # TeX 侧的兜底限制：不读/写工作目录以外的文件。
    env.update({'openin_any': 'p', 'openout_any': 'p', 'shell_escape': 'f'})
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    argv = ['--disable-installer', '-no-shell-escape', '-interaction=nonstopmode',
            '-halt-on-error', 'formula.tex']
    try:
        result = run([latex] + argv, cwd=work, env=env, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, timeout=COMPILE_TIMEOUT, creationflags=flags)
    except subprocess.TimeoutExpired as error:
        raise ValueError('LaTeX 编译超时，请简化公式') from error
    log = _decode_log(getattr(result, 'stdout', b''))
    if result.returncode == 0:
        return log
    if _option_error(log):
        raise ValueError('pdflatex 不接受 --disable-installer；本工具只支持 MiKTeX 的 pdfTeX，且禁止自动安装宏包')
    details = [line for line in log.splitlines() if line.startswith('!')]
    if not details:
        details = [line for line in log.splitlines() if line.strip()][-3:]
    raise ValueError('LaTeX 编译失败：' + ' | '.join(details)[:800])


def _render_alpha(pdfium, pdf_path):
    """把单页 PDF 渲染成 'L' 模式的 alpha 图，并抽取书写顺序原子。

    兼容性说明：pypdfium2 的 ``PdfDocument`` 支持 with 语句，但 ``PdfPage``
    在 5.x 里没有 ``__enter__``/``__exit__``，所以统一显式 close()。

    返回 ``(alpha, atoms, page_size, image_size)``；``atoms`` 为 None 时表示这一页
    拿不到可靠的字形/规则线结构，调用方就省略 writingPlan。
    """
    pdf = pdfium.PdfDocument(pdf_path)
    try:
        if len(pdf) != 1:
            raise ValueError('公式必须只生成一页')
        page = pdf[0]
        try:
            width, height = page.get_size()
            if width <= 0 or height <= 0:
                raise ValueError('公式页面尺寸无效')
            scale = RENDER_DPI / 72.0
            if (width * scale > MAX_WIDTH or height * scale > MAX_HEIGHT
                    or width * height * scale * scale > MAX_AREA):
                raise ValueError('公式渲染尺寸超过限制；请拆分为几条公式')
            # fill_color 的 alpha=0 让背景透明，字形留在 alpha 通道里。
            bitmap = page.render(scale=scale, fill_color=(255, 255, 255, 0))
            try:
                rendered = bitmap.to_pil()
                image_size = rendered.size
                alpha = rendered.convert('RGBA').getchannel('A')
            finally:
                bitmap.close()
            try:
                atoms = extract_atoms(pdfium, page)
            except Exception:  # noqa: BLE001 - 结构不可得时只是没有计划，不影响渲染
                atoms = None
            return alpha, atoms, (width, height), image_size
        finally:
            page.close()
    finally:
        pdf.close()


def _alpha_to_rle(alpha, Image):
    """裁剪空白、加 1px 边框并做行程编码。返回 (padded, raw, rle, crop)。"""
    box = alpha.getbbox()
    if not box:
        raise ValueError('公式没有可显示内容')
    alpha = alpha.crop(box)
    padded = Image.new('L', (alpha.width + 2, alpha.height + 2))
    padded.paste(alpha, (1, 1))
    if (padded.width > MAX_WIDTH or padded.height > MAX_HEIGHT
            or padded.width * padded.height > MAX_AREA):
        raise ValueError('公式渲染尺寸超过限制')
    raw = padded.tobytes()
    rle = []
    index = 0
    total = len(raw)
    while index < total:
        end = index + 1
        limit = min(index + 255, total)
        while end < limit and raw[end] == raw[index]:
            end += 1
        rle.extend((end - index, raw[index]))
        index = end
    return padded, raw, rle, box


def render_formula(source, *, pdfium=None, pil_image=None, runner=None):
    """渲染公式并返回结果字典；失败抛异常（由 main 转成 ok:false）。

    ``pdfium`` / ``pil_image`` / ``runner`` 是给单元测试注入的钩子，正常调用不用传。
    """
    validate_latex(source)
    if pil_image is None or pdfium is None:
        pil_image, pdfium = _load_render_deps()
    latex = shutil.which('pdflatex')
    if not latex:
        raise ValueError('找不到 pdflatex，请把 MiKTeX 的 bin 目录加入 PATH')
    run = runner or subprocess.run
    folder = _make_temp_dir('physics-formula-')
    try:
        work = Path(folder)
        (work / 'formula.tex').write_text(build_tex(source), encoding='ascii')
        _compile_tex(latex, work, run)
        alpha, atoms, page_size, image_size = _render_alpha(pdfium, work / 'formula.pdf')
        padded, raw, rle, crop = _alpha_to_rle(alpha, pil_image)
        name = 'latex_' + hashlib.sha256(source.encode('ascii') + raw).hexdigest()
        result = dict(ok=True, latex=source, name=name, width=padded.width,
                      height=padded.height, rle=rle, displayScale=.5)
        if atoms is not None:
            try:
                result['writingPlan'] = build_plan(atoms, raw, padded.width, padded.height,
                                                   (crop[0], crop[1]), page_size, image_size)
            except Exception:  # noqa: BLE001 - 计划只是可选元数据，推导失败就省略
                pass
        return result
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description='把受限数学 LaTeX 渲染成 alpha 掩码')
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.input.stat().st_size > MAX_REQUEST_BYTES:
            raise ValueError('公式请求过大')
        request = json.loads(args.input.read_text(encoding='utf-8'))
        if not isinstance(request, dict):
            raise ValueError('公式请求必须是 JSON 对象')
        result = render_formula(request['latex'])
    except Exception as error:  # noqa: BLE001 - CLI 必须把任何失败写成 ok:false
        result = dict(ok=False, error=str(error))
    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False), encoding='utf-8')
    except OSError:
        return 2
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    raise SystemExit(main())
