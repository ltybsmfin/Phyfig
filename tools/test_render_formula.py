"""render_formula 的单元测试。

* 校验部分（越界 / 注入 / 合法分段公式）只依赖标准库，随时可跑；
* 渲染部分用注入的假 pdfium/Pillow 验证契约与资源释放，不启动 pdflatex；
* 真实编译测试在缺少 pdflatex 或 Pillow/pypdfium2 时自动跳过；
* 形状回归把"紧页面"输出和在充足大页面上的参考渲染按 alpha 边界对齐，
  逐像素比较覆盖率与均值差，能抓住"只输出上半部"这类裁切错误。
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import types
import unittest
from unittest import mock

from render_formula import (MAX_SOURCE_BYTES, RENDER_DPI, build_tex,
                            render_formula, validate_latex, _compile_tex,
                            _make_temp_dir)

HAVE_PDFLATEX = shutil.which('pdflatex') is not None
HAVE_DEPS = (importlib.util.find_spec('PIL') is not None
             and importlib.util.find_spec('pypdfium2') is not None)


class ValidateLatexTest(unittest.TestCase):
    def test_accepts_segmented_math(self):
        formulas = [
            r'\vec{v}_0=\frac{\Delta x}{\Delta t}',
            r'x=\begin{cases}t^2&t>0\\0&t\leq0\end{cases}',
            r'\begin{aligned}a&=b+c\\d&=e-f\end{aligned}',
            r'\begin{pmatrix}a&b\\c&d\end{pmatrix}',
            r'\left(\frac{a}{b}\right)^{2}',
            r'\sum_{i=1}^{n}i^2=\frac{n(n+1)(2n+1)}{6}',
            r'\operatorname{argmax}_{x\in\mathcal{X}}f(x)',
            r'\alpha\;\beta\, \gamma\quad\delta',
            r'\hat{x}\ \mathrm{d}x',
            r'\begin{bmatrix}1&0\\0&1\end{bmatrix}',
        ]
        for formula in formulas:
            with self.subTest(formula=formula):
                self.assertEqual(formula, validate_latex(formula))

    def test_rejects_injection(self):
        formulas = [
            r'\input{secret}', r'\openin1=secret', r'\openout1=out',
            r'\read1to\a', r'\write18{cmd}', r'\immediate\write18{cmd}',
            r'\special{ps: }', r'\pdfliteral{x}', r'\directlua{os.execute("x")}',
            r'\csname input\endcsname', r'\def\a{x}', r'\newcommand{\x}{a}',
            r'\catcode1=2', r'\scantokens{x}', r'\expandafter\input\a',
            r'\the\count0', r'\uppercase{x}', r'\string x', r'\number1',
            r'\lstinputlisting{x}', r'\verb|x|', r'\endlinechar=13',
            r'\everypar{}', r'\toks0={x}', r'\shipout\hbox{x}', r'\hbox{x}',
            r'\vbox{x}', r'\kern1pt', r'\char65', r'\mathchoice{a}{b}{c}{d}',
            r'\usepackage{shellesc}', r'\begin{document}x\end{document}',
        ]
        for formula in formulas:
            with self.subTest(formula=formula), self.assertRaises(ValueError):
                validate_latex(formula)

    def test_rejects_environment_escape(self):
        formulas = [
            r'\begin{tabular}c\end{tabular}',
            r'\begin{align}a\end{align}',
            r'\text{\include{a}}',
            r'\begin{verbatim}x\end{verbatim}',
            r'\begin{lstlisting}x\end{lstlisting}',
        ]
        for formula in formulas:
            with self.subTest(formula=formula), self.assertRaises(ValueError):
                validate_latex(formula)

    def test_rejects_encoding_and_comment_tricks(self):
        formulas = [
            '公式', 'x\ty', 'x\x00y', '^^5cinput{a}', '^^', 'x%comment',
            '$x$', 'x#1', 'x`y', 'x\x7fy', 'x\x1by',
        ]
        for formula in formulas:
            with self.subTest(formula=repr(formula)), self.assertRaises(ValueError):
                validate_latex(formula)

    def test_rejects_structure(self):
        for formula in ['', '   ', '{x', 'x}', 'x\\', '\\', r'\frac{a}{b']:
            with self.subTest(formula=repr(formula)), self.assertRaises(ValueError):
                validate_latex(formula)

    def test_accepts_control_symbols(self):
        # \\（换行）等控制符号在数学环境里合法；只有孤立的尾部反斜杠才拒绝。
        for formula in [r'a\,b', r'a\;b', r'a\!b', r'\{x\}', r'\_', r'x\\y']:
            with self.subTest(formula=formula):
                self.assertEqual(formula, validate_latex(formula))

    def test_rejects_length_and_depth(self):
        with self.assertRaises(ValueError):
            validate_latex('x' * (MAX_SOURCE_BYTES + 1))
        too_deep = '{' * 33 + 'x' + '}' * 33
        with self.assertRaises(ValueError):
            validate_latex(too_deep)
        # 32 层仍然合法
        depth_ok = '{' * 32 + 'x' + '}' * 32
        self.assertEqual(depth_ok, validate_latex(depth_ok))


class BuildTexTest(unittest.TestCase):
    def test_sets_tight_page_size(self):
        source = r'\frac{a}{b}'
        tex = build_tex(source)
        for directive in ('\\pdfpagewidth=\\paperwidth', '\\pdfpageheight=\\paperheight',
                          '\\pdfhorigin=\\fmdim', '\\pdfvorigin=\\fmdim',
                          '\\shipout\\box\\fmbox', '\\setbox\\fmbox='):
            self.assertIn(directive, tex)
        self.assertIn(r'$\displaystyle \frac{a}{b}$', tex)

    def test_vorigin_is_only_the_margin(self):
        # 实测 \shipout\box 把 box 左上角放在 (horigin, vorigin)，与 ht/dp 无关；
        # vorigin 只要等于边距。写成 \ht+\fmdim 会把内容整体下移一个 box 高度，
        # 裁掉分母/下标（曾把公式只渲染出上半部）。
        tex = build_tex(r'x')
        self.assertIn('\\pdfvorigin=\\fmdim', tex)
        self.assertNotIn('\\pdfvorigin=\\dimexpr\\ht\\fmbox', tex)
        self.assertNotIn('\\pdfvorigin=\\dimexpr\\dp\\fmbox', tex)

    def test_never_uses_default_full_page(self):
        tex = build_tex(r'x')
        # 旧的 \shipout\hbox{...} 会让 MediaBox 停在默认 A4，必须不再出现。
        self.assertNotIn('\\shipout\\hbox', tex)
        self.assertNotIn('\\shipout\\vbox', tex)

    def test_output_is_ascii(self):
        tex = build_tex(r'\alpha+\beta')
        self.assertEqual(tex, tex.encode('ascii').decode('ascii'))

    def test_validates_before_building(self):
        with self.assertRaises(ValueError):
            build_tex(r'\input{x}')


class _FakeBitmap:
    def __init__(self, image):
        self.image = image
        self.closed = False

    def to_pil(self):
        return self.image

    def close(self):
        self.closed = True


class _FakePage:
    """故意不实现 __enter__/__exit__：渲染代码若把它当 contextmanager 会直接报错。"""

    def __init__(self, size, image):
        self.size = size
        self.image = image
        self.closed = False
        self.bitmap = None
        self.rendered = None

    def get_size(self):
        return self.size

    def render(self, scale=1, fill_color=None):
        self.rendered = (scale, fill_color)
        self.bitmap = _FakeBitmap(self.image)
        return self.bitmap

    def close(self):
        self.closed = True


class _FakeDocument:
    def __init__(self, path, page):
        self.path = path
        self.page = page
        self.closed = False

    def __len__(self):
        return 1

    def __getitem__(self, index):
        return self.page

    def close(self):
        self.closed = True


def _alpha_image(Image, width=4, height=3):
    image = Image.new('RGBA', (width, height), (0, 0, 0, 0))
    for x in range(width):
        image.putpixel((x, 0), (255, 0, 0, 255))
    return image


def _ok_runner():
    return mock.Mock(return_value=types.SimpleNamespace(returncode=0, stdout=b''))


@unittest.skipUnless(HAVE_DEPS, '需要 Pillow 与 pypdfium2')
class RenderMockedTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch('render_formula.shutil.which', return_value='pdflatex')
        self.addCleanup(patcher.stop)
        patcher.start()

    def test_returns_contract_and_closes_resources(self):
        from PIL import Image
        page = _FakePage((100.0, 40.0), _alpha_image(Image))
        document = _FakeDocument('formula.pdf', page)
        pdfium = types.SimpleNamespace(PdfDocument=lambda path: document)
        result = render_formula(r'\frac{a}{b}', pdfium=pdfium, pil_image=Image,
                                runner=_ok_runner())
        self.assertTrue(result['ok'])
        self.assertEqual(result['latex'], r'\frac{a}{b}')
        self.assertEqual((result['width'], result['height']), (6, 3))
        self.assertEqual(result['displayScale'], 0.5)
        self.assertTrue(result['name'].startswith('latex_'))
        self.assertEqual(len(result['name']), len('latex_') + 64)
        self.assertEqual(sum(result['rle'][::2]), result['width'] * result['height'])
        self.assertTrue(any(result['rle'][1::2]))
        self.assertEqual(page.rendered, (240 / 72.0, (255, 255, 255, 0)))
        self.assertTrue(document.closed)
        self.assertTrue(page.closed)
        self.assertTrue(page.bitmap.closed)

    def test_rejects_oversized_page_before_render(self):
        from PIL import Image
        page = _FakePage((2000.0, 2000.0), _alpha_image(Image))
        document = _FakeDocument('formula.pdf', page)
        pdfium = types.SimpleNamespace(PdfDocument=lambda path: document)
        with self.assertRaises(ValueError):
            render_formula(r'x', pdfium=pdfium, pil_image=Image, runner=_ok_runner())
        self.assertIsNone(page.rendered)
        self.assertTrue(document.closed)
        self.assertTrue(page.closed)

    def test_rejects_blank_render(self):
        from PIL import Image
        blank = Image.new('RGBA', (4, 3), (0, 0, 0, 0))
        page = _FakePage((20.0, 10.0), blank)
        document = _FakeDocument('formula.pdf', page)
        pdfium = types.SimpleNamespace(PdfDocument=lambda path: document)
        with self.assertRaises(ValueError):
            render_formula(r'x', pdfium=pdfium, pil_image=Image, runner=_ok_runner())
        self.assertTrue(page.bitmap.closed)

    def test_compile_failure_is_reported(self):
        from PIL import Image
        page = _FakePage((20.0, 10.0), _alpha_image(Image))
        document = _FakeDocument('formula.pdf', page)
        pdfium = types.SimpleNamespace(PdfDocument=lambda path: document)
        runner = mock.Mock(return_value=types.SimpleNamespace(
            returncode=1, stdout=b'! Undefined control sequence.'))
        with self.assertRaises(ValueError) as context:
            render_formula(r'\frac{a}{b}', pdfium=pdfium, pil_image=Image, runner=runner)
        self.assertIn('LaTeX 编译失败', str(context.exception))
        self.assertIsNone(page.rendered)

    def test_compile_is_miktex_only_and_does_not_retry(self):
        from PIL import Image
        page = _FakePage((20.0, 10.0), _alpha_image(Image))
        document = _FakeDocument('formula.pdf', page)
        pdfium = types.SimpleNamespace(PdfDocument=lambda path: document)
        runner = mock.Mock(return_value=types.SimpleNamespace(
            returncode=1, stdout=b'pdftex: unrecognized option --disable-installer'))
        with self.assertRaises(ValueError) as context:
            render_formula(r'\frac{a}{b}', pdfium=pdfium, pil_image=Image, runner=runner)
        self.assertIn('MiKTeX', str(context.exception))
        self.assertEqual(runner.call_count, 1)
        self.assertIn('--disable-installer', runner.call_args[0][0])


@unittest.skipUnless(HAVE_PDFLATEX and HAVE_DEPS, '需要 pdflatex 与 Pillow/pypdfium2')
class RenderIntegrationTest(unittest.TestCase):
    def test_renders_plain_formula(self):
        result = render_formula(r'\vec{v}_0=\frac{\Delta x}{\Delta t}')
        self.assertTrue(result['ok'])
        self.assertEqual(sum(result['rle'][::2]), result['width'] * result['height'])
        self.assertGreater(result['width'], 1)
        self.assertLessEqual(result['width'], 2048)
        self.assertLessEqual(result['height'], 1024)

    def test_renders_segmented_formula(self):
        result = render_formula(r'x=\begin{cases}t^2&t>0\\0&t\leq0\end{cases}')
        self.assertTrue(result['ok'])
        self.assertGreater(result['height'], 1)


def _rle_bytes(rle):
    data = bytearray()
    for index in range(0, len(rle), 2):
        data.extend(bytes([rle[index + 1]]) * rle[index])
    return bytes(data)


def _tight_alpha(result):
    """把 render_formula 的 rle 还原成去掉 1px 边框的 alpha（即内容 bbox 裁剪）。"""
    from PIL import Image
    image = Image.frombytes('L', (result['width'], result['height']), _rle_bytes(result['rle']))
    return image.crop((1, 1, result['width'] - 1, result['height'] - 1))


def _mask_agreement(tight, reference):
    """按左上角对齐，返回 (参考墨迹覆盖率, 紧掩码墨迹覆盖率, 平均绝对差)。

    覆盖率允许 1px 膨胀，以容忍亚像素抗锯齿差异；只要紧页面漏掉了下半部分
    （分母/下标），参考墨迹覆盖率就会明显下降。
    """
    from PIL import Image
    width, height = reference.size
    canvas = Image.new('L', (width, height), 0)
    canvas.paste(tight, (0, 0))
    merged = canvas.tobytes()
    reference_bytes = reference.tobytes()
    differences = [abs(a - b) for a, b in zip(merged, reference_bytes)]
    mean = sum(differences) / len(differences)

    tight_bytes = tight.tobytes()
    tight_width, tight_height = tight.size

    def tight_ink(x, y):
        return (0 <= x < tight_width and 0 <= y < tight_height
                and tight_bytes[y * tight_width + x] >= 128)

    def reference_ink(x, y):
        return (0 <= x < width and 0 <= y < height
                and reference_bytes[y * width + x] >= 128)

    reference_pixels = [(i % width, i // width)
                        for i, value in enumerate(reference_bytes) if value >= 128]
    tight_pixels = [(i % tight_width, i // tight_width)
                    for i, value in enumerate(tight_bytes) if value >= 128]

    def coverage(pixels, neighbour):
        if not pixels:
            return 1.0
        hit = 0
        for x, y in pixels:
            if any(neighbour(x + dx, y + dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
                hit += 1
        return hit / len(pixels)

    return coverage(reference_pixels, tight_ink), coverage(tight_pixels, reference_ink), mean


@unittest.skipUnless(HAVE_PDFLATEX and HAVE_DEPS, '需要 pdflatex 与 Pillow/pypdfium2')
class RenderShapeRegressionTest(unittest.TestCase):
    """紧页面输出必须与"大页面参考渲染 + alpha 裁切"的形状一致。"""

    REFERENCE_PAGE_PT = 600
    REFERENCE_ORIGIN_PT = 300

    def _reference_tex(self, source):
        return (
            '\\documentclass[12pt]{article}\n'
            '\\usepackage{amsmath,amssymb}\n'
            '\\pagestyle{empty}\n'
            '\\newbox\\fmbox\n'
            '\\newdimen\\fmdim\n'
            '\\begin{document}\n'
            '\\setbox\\fmbox=\\hbox{$\\displaystyle ' + source + '$}\n'
            '\\fmdim=6pt\n'
            '\\pdfpagewidth=' + str(self.REFERENCE_PAGE_PT) + 'pt\n'
            '\\pdfpageheight=' + str(self.REFERENCE_PAGE_PT) + 'pt\n'
            '\\pdfhorigin=' + str(self.REFERENCE_ORIGIN_PT) + 'pt\n'
            '\\pdfvorigin=' + str(self.REFERENCE_ORIGIN_PT) + 'pt\n'
            '\\shipout\\box\\fmbox\n'
            '\\end{document}\n'
        )

    def _reference_alpha(self, source):
        import pypdfium2 as pdfium
        folder = _make_temp_dir('formula-reference-')
        try:
            work = Path(folder)
            (work / 'formula.tex').write_text(self._reference_tex(source), encoding='ascii')
            _compile_tex('pdflatex', work, subprocess.run)
            pdf = pdfium.PdfDocument(work / 'formula.pdf')
            try:
                page = pdf[0]
                try:
                    bitmap = page.render(scale=RENDER_DPI / 72.0,
                                         fill_color=(255, 255, 255, 0))
                    try:
                        return bitmap.to_pil().convert('RGBA').getchannel('A')
                    finally:
                        bitmap.close()
                finally:
                    page.close()
            finally:
                pdf.close()
        finally:
            shutil.rmtree(folder, ignore_errors=True)

    def _assert_matches_reference(self, source):
        reference = self._reference_alpha(source)
        reference = reference.crop(reference.getbbox())
        tight = _tight_alpha(render_formula(source))
        self.assertLessEqual(abs(tight.width - reference.width), 1,
                             f'{source}: 宽度 {tight.width} vs 参考 {reference.width}')
        self.assertLessEqual(abs(tight.height - reference.height), 1,
                             f'{source}: 高度 {tight.height} vs 参考 {reference.height}（疑似被裁切）')
        reference_coverage, tight_coverage, mean = _mask_agreement(tight, reference)
        self.assertGreaterEqual(reference_coverage, 0.95,
                                f'{source}: 参考墨迹只覆盖 {reference_coverage:.3f}')
        self.assertGreaterEqual(tight_coverage, 0.95,
                                f'{source}: 紧掩码墨迹只覆盖 {tight_coverage:.3f}')
        self.assertLessEqual(mean, 30.0, f'{source}: 平均绝对差 {mean:.1f} 过大')

    def test_fraction_denominator_not_clipped(self):
        self._assert_matches_reference(r'\frac{\Delta x}{\Delta t}')

    def test_superscript_subscript_not_clipped(self):
        self._assert_matches_reference(r'v_0^2+x_i')

    def test_sqrt_not_clipped(self):
        self._assert_matches_reference(r'\sqrt{\frac{a}{b}}')

    def test_cases_not_clipped(self):
        self._assert_matches_reference(r'x=\begin{cases}t^2&t>0\\0&t\leq0\end{cases}')


if __name__ == '__main__':
    unittest.main()
