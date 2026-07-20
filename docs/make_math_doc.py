# -*- coding: utf-8 -*-
"""Генерирует PDF с полной математикой расчёта, как реализовано в krutiev."""
import textwrap

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages

PAGE_W, PAGE_H = 8.27, 11.69      # A4, дюймы
MARGIN_L, MARGIN_R = 0.85, 0.85
MARGIN_T, MARGIN_B = 0.75, 0.8

STYLES = {
    "title":  dict(size=19, weight="bold", indent=0.0, before=0.10, after=0.16, wrap=30),
    "sub":    dict(size=11, weight="normal", indent=0.0, before=0.02, after=0.30, wrap=78, color="0.25"),
    "h1":     dict(size=13.5, weight="bold", indent=0.0, before=0.26, after=0.10, wrap=56),
    "h2":     dict(size=11.5, weight="bold", indent=0.0, before=0.16, after=0.06, wrap=68),
    "body":   dict(size=9.8, weight="normal", indent=0.0, before=0.02, after=0.05, wrap=86),
    "item":   dict(size=9.8, weight="normal", indent=0.22, before=0.01, after=0.04, wrap=82),
    "math":   dict(size=11.5, weight="normal", indent=0.45, before=0.05, after=0.09, wrap=None),
    "mathsm": dict(size=10.2, weight="normal", indent=0.45, before=0.03, after=0.07, wrap=None),
    "note":   dict(size=8.4, weight="normal", indent=0.0, before=0.01, after=0.08, wrap=98, color="0.42"),
}

LINE_FACTOR = 1.55  # межстрочный множитель к размеру шрифта


class DocBuilder:
    def __init__(self, path):
        self.pdf = PdfPages(path)
        self.fig = None
        self.y = None
        self.page_number = 0

    def _new_page(self):
        if self.fig is not None:
            self._finish_page()
        self.page_number += 1
        self.fig = plt.figure(figsize=(PAGE_W, PAGE_H))
        self.y = PAGE_H - MARGIN_T

    def _finish_page(self):
        self.fig.text(0.5, MARGIN_B * 0.45 / PAGE_H,
                      "— %d —" % self.page_number,
                      ha="center", va="bottom", fontsize=8, color="0.45")
        self.fig.text(MARGIN_L / PAGE_W, MARGIN_B * 0.45 / PAGE_H,
                      "krutiev: методика ЭМ-расчёта",
                      ha="left", va="bottom", fontsize=7.2, color="0.6")
        self.pdf.savefig(self.fig)
        plt.close(self.fig)
        self.fig = None

    def _ensure(self, needed_inches):
        if self.fig is None or self.y - needed_inches < MARGIN_B:
            self._new_page()

    def add(self, style_name, text=""):
        style = STYLES[style_name]
        lines = [text]
        if style["wrap"] and text:
            lines = textwrap.wrap(text, style["wrap"]) or [""]
        line_height = style["size"] * LINE_FACTOR / 72.0
        block = style["before"] + line_height * len(lines) + style["after"]
        self._ensure(block)
        self.y -= style["before"]
        for line in lines:
            self.y -= line_height
            if line:
                self.fig.text((MARGIN_L + style["indent"]) / PAGE_W,
                              self.y / PAGE_H,
                              line,
                              ha="left", va="baseline",
                              fontsize=style["size"],
                              fontweight=style["weight"],
                              color=style.get("color", "black"))
        self.y -= style["after"]

    def rule(self):
        self._ensure(0.12)
        self.y -= 0.06
        self.fig.add_artist(plt.Line2D(
            [MARGIN_L / PAGE_W, 1 - MARGIN_R / PAGE_W],
            [self.y / PAGE_H, self.y / PAGE_H],
            color="0.6", linewidth=0.8, transform=self.fig.transFigure))
        self.y -= 0.06

    def close(self):
        if self.fig is not None:
            self._finish_page()
        self.pdf.close()


def build(path):
    d = DocBuilder(path)

    # ------------------------------------------------------------- титул
    d.add("title", "Математика расчёта полей в прямоугольном волноводе")
    d.add("sub", "Методика в точности повторяет реализацию программы krutiev: "
                 "аналитические решатели (em/rectangular_waveguide_solver, em/transverse_pec_partition_solver, "
                 "em/rectangular_mode_field), конечноэлементный решатель (em/mfem_frequency_domain_backend, "
                 "em/gmsh_tetrahedral_mesher) и постобработку (postprocessing/*). Все контрольные соотношения "
                 "закреплены тестами tests/em_core_tests.cpp.")
    d.rule()

    # ------------------------------------------------------------- 1
    d.add("h1", "1. Геометрия, соглашения и материал")
    d.add("body", "Ось z направлена вдоль волновода, начало координат — в центре полости. Внутренние размеры "
                  "получаются из внешних вычитанием двух толщин стенки:")
    d.add("math", r"$a = W - 2t_w,\qquad b = H - 2t_w,\qquad x\in[-a/2,\,a/2],\ \ y\in[-b/2,\,b/2],\ \ z\in[-L/2,\,L/2]$")
    d.add("body", "Все поля — комплексные фазоры с временной зависимостью exp(+jωt); мгновенное значение "
                  "восстанавливается как")
    d.add("math", r"$\mathbf{F}(\mathbf{r},t) = \mathrm{Re}\left[\,\mathbf{F}(\mathbf{r})\,e^{\,j\omega t}\right],\qquad \omega = 2\pi f$")
    d.add("body", "Заполнение — пассивная среда с комплексной диэлектрической проницаемостью (проводимость σ "
                  "учитывается мнимой добавкой) и вещественной магнитной проницаемостью:")
    d.add("math", r"$\varepsilon_c = \varepsilon_0\varepsilon_r - j\,\dfrac{\sigma}{\omega},\qquad \mu = \mu_0\mu_r,\qquad k^2 = \omega^2\mu\,\varepsilon_c$")
    d.add("note", "Код: waveguide_calculator.cpp (перевод мм → м, построение модели), em/em_model.h, em/rectangular_mode_field.cpp.")

    # ------------------------------------------------------------- 2
    d.add("h1", "2. Спектр мод TEmn / TMmn")
    d.add("body", "Для каждой пары индексов (m, n) поперечные волновые числа и частота отсечки:")
    d.add("math", r"$k_x = \dfrac{m\pi}{a},\qquad k_y = \dfrac{n\pi}{b},\qquad k_c^2 = k_x^2 + k_y^2,\qquad f_c = \dfrac{c\,k_c}{2\pi\sqrt{\varepsilon_r\mu_r}}$")
    d.add("body", "Продольная постоянная распространения берётся как «пассивная» ветвь квадратного корня "
                  "(Re γ ≥ 0, а при Re γ = 0 выбирается Im γ ≥ 0):")
    d.add("math", r"$\gamma = \sqrt{\,k_c^2 - \omega^2\mu\,\varepsilon_c\,} = \alpha + j\beta,\qquad \alpha=\mathrm{Re}\,\gamma\ \ [\mathrm{Np/m}],\quad \beta=\mathrm{Im}\,\gamma\ \ [\mathrm{rad/m}]$")
    d.add("body", "Мода распространяется при f > fc. Длина волны в свободном пространстве и в волноводе:")
    d.add("math", r"$\lambda_0 = c/f,\qquad \lambda_g = 2\pi/\beta$")
    d.add("item", "— TE-моды существуют при (m,n) ≠ (0,0); TM-моды требуют m ≥ 1 и n ≥ 1;")
    d.add("item", "— при автоматическом выборе берётся первая распространяющаяся мода в порядке роста fc "
                  "(при равенстве отсечек TE предшествует TM); обычно это TE10.")
    d.add("note", "Код: em/rectangular_waveguide_solver.cpp (enumerateModes, passiveSquareRoot). Тест: невязка β для TE10 < 1e-12.")

    # ------------------------------------------------------------- 3
    d.add("h1", "3. Поля мод (суперпозиция прямой и обратной волн)")
    d.add("body", "Вводятся локальные координаты x̃ = x + a/2 ∈ [0, a], ỹ = y + b/2 ∈ [0, b], z̃ = z + L/2 ∈ [0, L] "
                  "и продольный фактор с амплитудами прямой (A) и обратной (B) волн:")
    d.add("math", r"$f(\tilde z) = A\,e^{-\gamma\tilde z} + B\,e^{+\gamma\tilde z},\qquad f'(\tilde z) = \gamma\left(B\,e^{+\gamma\tilde z} - A\,e^{-\gamma\tilde z}\right)$")
    d.add("h2", "3.1. TE-моды (Hz ≠ 0, Ez = 0)")
    d.add("math", r"$\psi(\tilde x,\tilde y) = \cos(k_x\tilde x)\,\cos(k_y\tilde y),\qquad H_z = f(\tilde z)\,\psi$")
    d.add("math", r"$\mathbf{H}_t = \dfrac{f'(\tilde z)}{k_c^2}\,\nabla_t\psi,\qquad E_x = -\,\dfrac{j\omega\mu}{k_c^2}\,f\,\dfrac{\partial\psi}{\partial \tilde y},\qquad E_y = +\,\dfrac{j\omega\mu}{k_c^2}\,f\,\dfrac{\partial\psi}{\partial \tilde x}$")
    d.add("h2", "3.2. TM-моды (Ez ≠ 0, Hz = 0)")
    d.add("math", r"$\varphi(\tilde x,\tilde y) = \sin(k_x\tilde x)\,\sin(k_y\tilde y),\qquad E_z = f(\tilde z)\,\varphi$")
    d.add("math", r"$\mathbf{E}_t = \dfrac{f'(\tilde z)}{k_c^2}\,\nabla_t\varphi,\qquad H_x = +\,\dfrac{j\omega\varepsilon_c}{k_c^2}\,f\,\dfrac{\partial\varphi}{\partial \tilde y},\qquad H_y = -\,\dfrac{j\omega\varepsilon_c}{k_c^2}\,f\,\dfrac{\partial\varphi}{\partial \tilde x}$")
    d.add("body", "Эти выражения удовлетворяют уравнениям Максвелла rot E = −jωμH, rot H = +jωε_c E и граничным "
                  "условиям на идеально проводящих стенках (касательное E и нормальное H обращаются в ноль).")
    d.add("note", "Код: em/rectangular_mode_field.cpp. Тесты: численный ротор — невязки Фарадея и Ампера < 2e-7; "
                  "касательное E на стенках < 1e-9.")

    # ------------------------------------------------------------- 4
    d.add("h1", "4. Нормировка мощности возбуждения")
    d.add("body", "Средний по времени вектор Пойнтинга и мощность через сечение z = const:")
    d.add("math", r"$\bar{\mathbf{S}} = \dfrac{1}{2}\,\mathrm{Re}\left(\mathbf{E}\times\mathbf{H}^{*}\right),\qquad P(z) = \int\int \bar S_z\;dx\,dy$")
    d.add("body", "Интеграл берётся квадратурой средних точек на сетке 80×56. Для распространяющейся моды амплитуда "
                  "выбирается так, чтобы падающая волна несла ровно P0 = 1 Вт на входном порту z1 = −L/2 "
                  "(P_unit — мощность при единичной амплитуде):")
    d.add("math", r"$A = \sqrt{P_0 / P_{unit}}$")
    d.add("note", "Код: em/rectangular_waveguide_solver.cpp (integrateForwardPower). Тест: P(z1) = 1 ± 2e-12 Вт для TE10 и TM11.")

    # ------------------------------------------------------------- 5
    d.add("h1", "5. Пустой волновод: S-параметры и баланс мощности")
    d.add("math", r"$S_{21} = S_{12} = e^{-\gamma L},\qquad S_{11} = S_{22} = 0$")
    d.add("math", r"$P_{out} = P_{in}\,e^{-2\alpha L},\qquad P_{diss} = P_{in} - P_{out}$")
    d.add("body", "Контроль: относительная ошибка баланса мощности берётся как максимум из ошибки закона сохранения "
                  "и расхождения P(z2) с P(z1)·|S21|².")
    d.add("note", "Тесты: |S21| = 1 (без потерь), реципрокность S12 = S21; при σ > 0 — α > 0, P_diss > 0, баланс < 2e-12.")

    # ------------------------------------------------------------- 6
    d.add("h1", "6. Сплошная поперечная PEC-перегородка (аналитический решатель)")
    d.add("body", "Применяется, когда включена ровно одна пластина без поворотов, перекрывающая всё сечение "
                  "(в пределах допуска 1e-10 м) и лежащая строго между портами; щелей нет. Пластина занимает "
                  "z ∈ [z_f, z_b]. Расстояние от входного порта до её передней грани:")
    d.add("math", r"$d = z_f + L/2$")
    d.add("body", "На передней грани идеального проводника касательное электрическое поле обращается в ноль. "
                  "Для TE-мод E_t ∝ f, для TM-мод E_t ∝ f′, поэтому условие короткого замыкания даёт разные "
                  "знаки отражения продольной амплитуды:")
    d.add("math", r"$\mathrm{TE}:\ f(d)=0\ \Rightarrow\ B = -A\,e^{-2\gamma d}\qquad\quad \mathrm{TM}:\ f'(d)=0\ \Rightarrow\ B = +A\,e^{-2\gamma d}$")
    d.add("body", "В конвенции поперечного электрического поля коэффициент отражения на входном порту одинаков "
                  "для обоих семейств (короткое замыкание, пересчитанное на плоскость порта):")
    d.add("math", r"$S_{11} = -\,e^{-2\gamma d},\qquad S_{21} = S_{12} = 0,\qquad S_{22} = -\,e^{-2\gamma d'},\quad d' = L/2 - z_b$")
    d.add("body", "Поле восстанавливается только в области [−L/2, z_f] как стоячая волна (раздел 3 с найденным B); "
                  "внутри металла и за перегородкой поле тождественно равно нулю. Мощности (P_diss — потери "
                  "заполняющей среды на пути 2d туда и обратно):")
    d.add("math", r"$P_{refl} = |S_{11}|^2 P_{inc},\qquad P_{diss} = P_{inc} - P_{refl},\qquad P_{trans}=0$")
    d.add("body", "Автоматически выполняются граничные условия: у TE на грани H_z = f·ψ = 0 (нормальное H), у TM "
                  "остаётся E_z ≠ 0 (нормальное E — допустимо, поверхностный заряд) и H_t ≠ 0 (поверхностный ток).")
    d.add("note", "Код: em/transverse_pec_partition_solver.cpp. Тесты: фаза S11 = −exp(−2γd) с точностью 2e-12; "
                  "касательное E на грани < 2e-12 от максимума; поле за перегородкой = 0; |S11| = 1 без потерь.")

    # ------------------------------------------------------------- 7
    d.add("h1", "7. Маршрутизация решателей")
    d.add("item", "— нет включённых пластин и диэлектриков (щель — в приближении невозмущённого фона) → "
                  "аналитический решатель пустого волновода (разделы 2–5);")
    d.add("item", "— одна сплошная поперечная пластина → аналитический решатель перегородки (раздел 6);")
    d.add("item", "— частичные пластины (диафрагмы, штыри), несколько пластин, диэлектрики или строгий расчёт "
                  "щели → конечноэлементный решатель (разделы 8–12).")
    d.add("note", "Код: em/em_solver_dispatcher.cpp. Для щели по умолчанию поле фона не возмущается; это явно "
                  "сообщается предупреждением в панели результатов.")

    # ------------------------------------------------------------- 8
    d.add("h1", "8. Конечноэлементная постановка (MFEM, элементы Неделека)")
    d.add("body", "Решается векторное волновое уравнение для электрического поля:")
    d.add("math", r"$\nabla\times\left(\mu^{-1}\,\nabla\times\mathbf{E}\right) - \omega^2\varepsilon_c\,\mathbf{E} = 0$")
    d.add("body", "Слабая форма с портовыми граничными условиями первого порядка на торцах Γ1 (вход) и Γ2 (выход) "
                  "и возбуждением падающей модой на Γ1:")
    d.add("mathsm", r"$\left(\mu^{-1}\nabla\times\mathbf{E},\,\nabla\times\mathbf{v}\right) - \omega^2\left(\varepsilon_c\mathbf{E},\,\mathbf{v}\right) + \dfrac{j\beta}{\mu}\left\langle \mathbf{E}_t,\mathbf{v}_t\right\rangle_{\Gamma_1\cup\Gamma_2} = \dfrac{2j\beta}{\mu}\left\langle \mathbf{E}^{inc},\mathbf{v}\right\rangle_{\Gamma_1}$")
    d.add("body", "Здесь E_inc — нормированный на 1 Вт модовый профиль (раздел 4), вычисленный в плоскости входного "
                  "порта, а β — фазовая постоянная выбранной моды:")
    d.add("math", r"$\beta = \sqrt{\,k_0^2\,\mathrm{Re}(\varepsilon_r\mu_r) - k_c^2\,},\qquad k_0 = \omega/c$")
    d.add("body", "Обоснование портового слагаемого: для уходящей волны E_t ∝ exp(−jβz) выполняется "
                  "n̂×(μ⁻¹∇×E) = (jβ/μ)E_t, поэтому замена точного граничного члена массовым слагаемым точно "
                  "поглощает выбранную моду, а встречные волны отфильтровывает. На стенках и пластинах — "
                  "существенное условие идеального проводника (физическая поверхность 103):")
    d.add("math", r"$\hat{\mathbf{n}}\times\mathbf{E} = 0$")
    d.add("body", "Пространство дискретизации — H(curl)-согласованные элементы Неделека 1-го порядка на "
                  "тетраэдрах; магнитное поле восстанавливается из решения:")
    d.add("math", r"$\mathbf{H} = \dfrac{j}{\omega\mu}\,\nabla\times\mathbf{E}$")
    d.add("note", "Код: em/mfem_frequency_domain_backend.cpp. Тест: в пустом волноводе |S11| < 0.12, ||S21|−1| < 0.12, "
                  "пучность TE10 в центре сечения.")

    # ------------------------------------------------------------- 9
    d.add("h1", "9. Сетка (Gmsh) и автоматический шаг")
    d.add("body", "Геометрия строится булевыми операциями OpenCASCADE: полость минус включённые PEC-пластины "
                  "(их поверхности становятся границей 103), диэлектрические блоки конформно фрагментируются "
                  "(объёмы 2, 3, …), торцы получают метки портов 101 и 102. Автоматический максимальный шаг:")
    d.add("math", r"$h = \min\left(\dfrac{a}{8},\ \dfrac{b}{5},\ \dfrac{\lambda_m}{12}\right),\qquad \lambda_m = \dfrac{c}{f\,\sqrt{\max(1,\,|\varepsilon_r\mu_r|)}}$")
    d.add("body", "Ограничение b/5 выбрано по данным: более мелкая сетка (b/8) порождает СЛАУ, на которой "
                  "итерационный решатель застаивается около невязки 1e-4, при этом S-параметры сошедшегося "
                  "решения меняются менее чем на 0.2%.")
    d.add("note", "Код: em/gmsh_tetrahedral_mesher.cpp (buildGeometryScript, automaticMeshSize). При щели вокруг "
                  "волновода добавляется внешняя область с PML (раздел 11).")

    # ------------------------------------------------------------- 10
    d.add("h1", "10. Решение СЛАУ и критерии приёмки")
    d.add("body", "Комплексная система A·x = b (A = A_r + jA_i) собирается в эрмитовой блочной форме 2×2 "
                  "по вещественной и мнимой частям и решается GMRES с блочно-диагональным предобуславливателем: "
                  "один шаг Гаусса–Зейделя по вещественной положительно определённой форме "
                  "K+ = |μ⁻¹|·curl-curl + |ω²ε|·mass, у мнимого блока — знак минус:")
    d.add("mathsm", r"$M = \mathrm{diag}\left(GS(K_{+}),\ -GS(K_{+})\right)$")
    d.add("item", "— попытка 1: GMRES(200), допуск 1e-6, до 1200 итераций;")
    d.add("item", "— повтор: GMRES(500) с холодного старта, до max(6000, 3·1200) итераций, ослабленная цель "
                  "в пределах 1e-4 … 1e-3;")
    d.add("item", "— приёмка: явная относительная невязка r = ||b − A·x|| / ||b|| ≤ 1e-3 — результат принимается; "
                  "если r превышает ослабленную цель, добавляется предупреждение «поля и S-параметры "
                  "приближённые»; r > 1e-3 — ошибка расчёта.")
    d.add("body", "Обоснование порога: на контрольной задаче (штырь 0.5 мм в WR-90) решение с невязкой 1.2e-5 "
                  "даёт |S11|, совпадающий с полностью сошедшимся (3e-8) во всех четырёх знаках; невязка ~4e-4 "
                  "меняет |S11| на единицы процентов — это и фиксирует предупреждение.")
    d.add("note", "Код: em/mfem_frequency_domain_backend.cpp (GMRES, SolveProgressMonitor — прогресс каждые 100 итераций).")

    # ------------------------------------------------------------- 11
    d.add("h1", "11. PML для излучения щели")
    d.add("body", "При строгом расчёте щели вокруг волновода строится воздушная подушка и идеально согласованный "
                  "слой (PML) толщиной T = 0.35λ0 по осям x и y. Используется комплексное растяжение координат "
                  "с полиномиальным профилем степени p − 1 (p = 3):")
    d.add("math", r"$s(d) = 1 - j\,\dfrac{p\,\sigma_0}{k\,T^{p}}\,d^{\,p-1},\qquad \sigma_0 = -\dfrac{1}{2}\ln R,\qquad 0 \leq d \leq T$")
    d.add("body", "где d — глубина в слое, R — целевой коэффициент отражения (1e-8). Знак мнимой части отрицателен, "
                  "что для конвенции exp(+jωt) обеспечивает затухание уходящей волны exp(−jksd). Материальные "
                  "тензоры анизотропного PML:")
    d.add("math", r"$\Lambda = \mathrm{diag}\left(\dfrac{s_y s_z}{s_x},\ \dfrac{s_x s_z}{s_y},\ \dfrac{s_x s_y}{s_z}\right),\qquad \tilde\mu = \mu\,\Lambda,\qquad \tilde\varepsilon = \varepsilon_c\,\Lambda$")
    d.add("body", "Интеграл профиля подобран так, что двойной проход слоя ослабляет волну до уровня R "
                  "(интеграл k·|Im s| по толщине равен σ0, и exp(−2σ0) = R). Контроль физичности: "
                  "|S11|² + |S21|² ≤ 1 — доля излучения неотрицательна.")
    d.add("note", "Код: em/mfem_frequency_domain_backend.cpp (stretch, MaxwellMatrixCoefficient). Тест (KRUTIEV_RUN_FEM_SLOT_TEST): "
                  "ненулевое внешнее поле и пассивный баланс мощности.")

    # ------------------------------------------------------------- 12
    d.add("h1", "12. S-параметры конечноэлементного решения")
    d.add("body", "Амплитуда моды извлекается проекцией поля на модовый профиль входного порта e_ref по сетке "
                  "36×24 точек сечения:")
    d.add("math", r"$p(z) = \dfrac{\sum \mathbf{E}(x,y,z)\cdot \mathbf{e}_{ref}^{*}(x,y)}{\sum \left|\mathbf{e}_{ref}(x,y)\right|^{2}}$")
    d.add("body", "Отсчёт ведётся в плоскостях, смещённых внутрь на δ = max(1e-8 м, 1e-6·L). Полное поле на входе — "
                  "сумма падающей и отражённой волн, поэтому:")
    d.add("math", r"$S_{11} = \left(p(z_1{+}\delta) - e^{-j\beta\delta}\right)e^{-j\beta\delta},\qquad S_{21} = p(z_2{-}\delta)\,e^{+j\beta (L-\delta)}$")
    d.add("math", r"$P_{refl} = |S_{11}|^2 P_{inc},\qquad P_{trans} = |S_{21}|^2 P_{inc},\qquad P_{diss} = \max\left(0,\ P_{inc}-P_{refl}-P_{trans}\right)$")
    d.add("body", "Ортогональность модовых профилей в L2 гарантирует, что примесь высших мод не искажает проекцию; "
                  "возбуждённые у препятствия затухающие моды к плоскостям отсчёта уже пренебрежимы. Фаза S21 "
                  "приведена к плоскости входного порта (электрическая длина волновода исключена).")
    d.add("note", "Код: em/mfem_frequency_domain_backend.cpp (projectPortElectric). Быстрый поиск тетраэдра для точки — "
                  "равномерная сетка ячеек по ограничивающим параллелепипедам элементов (buildElementGrid).")

    # ------------------------------------------------------------- 13
    d.add("h1", "13. Постобработка и визуализация")
    d.add("h2", "13.1. Производные величины")
    d.add("math", r"$\mathbf{J}_s = \hat{\mathbf{n}}\times\mathbf{H},\qquad \bar{\mathbf{S}} = \dfrac{1}{2}\,\mathrm{Re}\left(\mathbf{E}\times\mathbf{H}^{*}\right)$")
    d.add("body", "J_s — поверхностный ток на металле, S̄ — средний поток мощности. Мгновенные картины строятся "
                  "при фазе φ = π/4: F(r) → Re[F(r)·exp(jφ)]. Ток снимается в точке, смещённой от металла внутрь "
                  "области на ~1e-6 от меньшего размера сечения.")
    d.add("h2", "13.2. Стрелки электрического поля моды TE10")
    d.add("body", "Стрелки размещаются в 11 сечениях z ∈ [−0.45L, 0.45L]. Внутри сечения 13 позиций x выбраны "
                  "равнопоточно — плотность стрелок повторяет распределение |sin(πx̃/a)|:")
    d.add("math", r"$q_k = \dfrac{k+1/2}{13},\qquad \theta_k = \arccos(1-2q_k),\qquad x_k = a\left(\dfrac{\theta_k}{\pi} - \dfrac{1}{2}\right),\quad k = 0\ldots12$")
    d.add("body", "Длина стрелки пропорциональна локальной амплитуде |E_y| (нормированной на максимум по объёму), "
                  "направление — знаку мгновенного значения; позиции с амплитудой ниже 2.5% максимума и точки "
                  "внутри металла пропускаются. Так стоячая волна перед препятствием и «тень» за ним видны вдоль "
                  "всей длины волновода.")
    d.add("h2", "13.3. Линии поля и токов")
    d.add("body", "Линии E, H и J_s интегрируются методом Рунге–Кутты 4-го порядка по единичному полю направлений "
                  "с шагом ~min(a, b, L)/82 (объём) и ~/72…/75 (поверхности); трассировка останавливается на "
                  "границе области, в металле, при падении амплитуды ниже 2.5% максимума или при замыкании линии.")
    d.add("h2", "13.4. Оценка возбуждения щели")
    d.add("body", "Нормированная связь щели с волной — произведение доли поверхностного тока, пересекающего щель "
                  "поперёк (в её центре r_c), и резонансного фактора конечной длины (ŵ — поперечная ось щели, "
                  "повёрнутой на угол θ):")
    d.add("math", r"$k_{slot} = \dfrac{\left|\mathbf{J}_s(\mathbf{r}_c)\cdot \hat{\mathbf{w}}\right|}{\max\left|\mathbf{J}_s\right|}\cdot\left|\,\mathrm{sinc}\left(\dfrac{\beta\,l_{slot}}{2}\right)\right|,\qquad \hat{\mathbf{w}} = \hat{\mathbf{u}}\cos\theta - \hat{\mathbf{z}}\sin\theta$")
    d.add("note", "Код: postprocessing/field_visualization_generator.cpp, postprocessing/slot_excitation_estimator.cpp. "
                  "Это инженерная оценка первого порядка; строгое рассеяние щелью считает FEM.")

    # ------------------------------------------------------------- 14
    d.add("h1", "14. Числовой пример: WR-90 по умолчанию, f = 10 ГГц")
    d.add("body", "Внешние размеры 22.86 × 10.16 мм, стенка 0.1 мм, длина 50 мм. Внутренние размеры и мода TE10:")
    d.add("mathsm", r"$a = 22.66\ \mathrm{mm},\qquad b = 9.96\ \mathrm{mm},\qquad f_c^{TE10} = \dfrac{c}{2a} = 6.615\ \mathrm{GHz}$")
    d.add("mathsm", r"$k_0 = \dfrac{2\pi f}{c} = 209.58\ \mathrm{rad/m},\qquad k_c = \dfrac{\pi}{a} = 138.63\ \mathrm{rad/m}$")
    d.add("mathsm", r"$\beta = \sqrt{k_0^2 - k_c^2} = 157.18\ \mathrm{rad/m},\qquad \lambda_0 = 29.98\ \mathrm{mm},\qquad \lambda_g = \dfrac{2\pi}{\beta} = 39.97\ \mathrm{mm}$")
    d.add("body", "Контрольная задача о центральном штыре 0.5 × 9.96 × 0.5 мм: сходимость по сетке "
                  "|S11| = 0.7384 (h = 2.0 мм) против 0.7377 (h = 2.5 мм); баланс |S11|² + |S21|² = 0.992 "
                  "при нулевых потерях — дефицит отражает ошибку дискретизации.")

    # ------------------------------------------------------------- 15
    d.add("h1", "15. Сводка контролируемых инвариантов")
    d.add("item", "1. Невязки уравнений Максвелла для модовых полей (численный ротор) — < 2e-7.")
    d.add("item", "2. Касательное E на идеальном проводнике (стенки, перегородка) — на уровне машинного нуля.")
    d.add("item", "3. Нормировка падающей мощности — 1 Вт с точностью 2e-12.")
    d.add("item", "4. Фаза и модуль S11 короткого замыкания — аналитическое значение −exp(−2γd), 2e-12.")
    d.add("item", "5. Баланс мощности: P_inc = P_refl + P_trans + P_diss (+ излучение ≥ 0 при щели).")
    d.add("item", "6. Пассивность: |S11|² + |S21|² ≤ 1; у пассивных сред α ≥ 0.")
    d.add("item", "7. Явная невязка СЛАУ ||b − A·x||/||b|| — печатается в диагностике и ограничена 1e-3.")

    d.close()


if __name__ == "__main__":
    import sys
    build(sys.argv[1] if len(sys.argv) > 1 else "math_doc.pdf")
    print("OK")
