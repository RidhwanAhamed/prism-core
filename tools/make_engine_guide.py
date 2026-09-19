#!/usr/bin/env python3
# Generates "Prism Core - Engine Guide" as a PDF (repo root: Prism-Core-Engine-Guide.pdf).
#   python3 tools/make_engine_guide.py      (needs: pip install reportlab)
# The Part II tables are NOT computed here: they were produced by driving the real
# engine, so they must be re-measured, not re-typed, when the engine changes.
# All engine numbers in this document come from the real code at commit 2f55a27
# (see scratchpad/engine_probe.txt for the run that produced the tables).

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT, TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (BaseDocTemplate, Frame, PageTemplate, Paragraph, Spacer,
                                Table, TableStyle, PageBreak, KeepTogether, CondPageBreak)

F = "/usr/share/fonts/truetype/dejavu/"
pdfmetrics.registerFont(TTFont("DJ", F + "DejaVuSans.ttf"))
pdfmetrics.registerFont(TTFont("DJ-B", F + "DejaVuSans-Bold.ttf"))
pdfmetrics.registerFont(TTFont("DJM", F + "DejaVuSansMono.ttf"))
pdfmetrics.registerFont(TTFont("DJM-B", F + "DejaVuSansMono-Bold.ttf"))
pdfmetrics.registerFontFamily("DJ", normal="DJ", bold="DJ-B", italic="DJ", boldItalic="DJ-B")

INK = colors.HexColor("#14181F")
MUTED = colors.HexColor("#5A6472")
ACCENT = colors.HexColor("#1F4E79")
RULE = colors.HexColor("#C9D1DA")
BAND = colors.HexColor("#EEF2F6")
WARN = colors.HexColor("#8A4B08")
WARNBG = colors.HexColor("#FDF3E7")
CODEBG = colors.HexColor("#F4F6F8")

PW, PH = A4
MARGIN = 20 * mm
CW = PW - 2 * MARGIN

S = {}
S["title"] = ParagraphStyle("title", fontName="DJ-B", fontSize=26, leading=31, textColor=INK,
                            spaceAfter=6)
S["subtitle"] = ParagraphStyle("subtitle", fontName="DJ", fontSize=13, leading=18,
                               textColor=MUTED, spaceAfter=18)
S["h1"] = ParagraphStyle("h1", fontName="DJ-B", fontSize=17, leading=21, textColor=ACCENT,
                         spaceBefore=2, spaceAfter=9)
S["h2"] = ParagraphStyle("h2", fontName="DJ-B", fontSize=12.5, leading=16, textColor=INK,
                         spaceBefore=13, spaceAfter=5)
S["h3"] = ParagraphStyle("h3", fontName="DJ-B", fontSize=10.5, leading=14, textColor=INK,
                         spaceBefore=9, spaceAfter=3)
S["body"] = ParagraphStyle("body", fontName="DJ", fontSize=9.4, leading=14.2, textColor=INK,
                           spaceAfter=7, alignment=TA_LEFT)
S["small"] = ParagraphStyle("small", fontName="DJ", fontSize=8.3, leading=12, textColor=MUTED,
                            spaceAfter=6)
S["bullet"] = ParagraphStyle("bullet", parent=S["body"], leftIndent=11, bulletIndent=2,
                             spaceAfter=3.5)
S["code"] = ParagraphStyle("code", fontName="DJM", fontSize=7.9, leading=11.2, textColor=INK)
S["cell"] = ParagraphStyle("cell", fontName="DJ", fontSize=8.1, leading=11, textColor=INK)
S["cellb"] = ParagraphStyle("cellb", fontName="DJ-B", fontSize=8.1, leading=11, textColor=INK)
S["cellm"] = ParagraphStyle("cellm", fontName="DJM", fontSize=7.4, leading=10.2, textColor=INK)
S["cellhead"] = ParagraphStyle("cellhead", fontName="DJ-B", fontSize=8.1, leading=11,
                               textColor=colors.white)
S["callout"] = ParagraphStyle("callout", fontName="DJ", fontSize=8.8, leading=13,
                              textColor=INK)

story = []


def P(t, s="body"):
    story.append(Paragraph(t, S[s]))


def H1(t):
    story.append(CondPageBreak(60 * mm))
    story.append(Paragraph(t, S["h1"]))
    story.append(Table([[""]], colWidths=[CW], rowHeights=[1.4],
                       style=TableStyle([("BACKGROUND", (0, 0), (-1, -1), ACCENT)])))
    story.append(Spacer(1, 9))


def H2(t):
    story.append(CondPageBreak(30 * mm))
    story.append(Paragraph(t, S["h2"]))


def H3(t):
    story.append(CondPageBreak(24 * mm))
    story.append(Paragraph(t, S["h3"]))


def BL(items, style="bullet"):
    for it in items:
        story.append(Paragraph(it, S[style], bulletText="•"))
    story.append(Spacer(1, 4))


def NL(items):
    for i, it in enumerate(items, 1):
        story.append(Paragraph(it, S["bullet"], bulletText="%d." % i))
    story.append(Spacer(1, 4))


def SP(h=6):
    story.append(Spacer(1, h))


def code(text, width=CW):
    lines = text.strip("\n").split("\n")
    rows = [[Paragraph(ln.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                       .replace(" ", " "), S["code"])] for ln in lines]
    t = Table(rows, colWidths=[width])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), CODEBG),
        ("BOX", (0, 0), (-1, -1), 0.5, RULE),
        ("LEFTPADDING", (0, 0), (-1, -1), 7), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 0.6), ("BOTTOMPADDING", (0, 0), (-1, -1), 0.6),
        ("TOPPADDING", (0, 0), (-1, 0), 6), ("BOTTOMPADDING", (0, -1), (-1, -1), 6),
    ]))
    # A code block travels with the heading that introduces it: pull any trailing
    # heading (and its CondPageBreak) off the story and keep them together.
    group = []
    while story:
        last = story[-1]
        if isinstance(last, Paragraph) and getattr(last, "style", None) is not None \
                and last.style.name in ("h2", "h3"):
            group.insert(0, story.pop())
            while story and isinstance(story[-1], CondPageBreak):
                story.pop()
            break
        if isinstance(last, Spacer) and last.height <= 6:
            group.insert(0, story.pop())
            continue
        break
    group.append(t)
    story.append(KeepTogether(group) if len(group) > 1 or len(lines) < 45 else t)
    SP(8)


def table(head, rows, widths, mono_cols=(), align_right=(), font_size=None, header=True):
    cs = S["cell"] if font_size is None else ParagraphStyle(
        "c%s" % font_size, parent=S["cell"], fontSize=font_size, leading=font_size + 2.8)
    cm = S["cellm"] if font_size is None else ParagraphStyle(
        "m%s" % font_size, parent=S["cellm"], fontSize=font_size - 0.6, leading=font_size + 2.4)
    data = [[Paragraph(h, S["cellhead"]) for h in head]] if header else []
    for r in rows:
        data.append([Paragraph(str(c), cm if i in mono_cols else cs) for i, c in enumerate(r)])
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    first = 1 if header else 0
    style = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.4, RULE),
        ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 3.6), ("BOTTOMPADDING", (0, 0), (-1, -1), 3.6),
        ("ROWBACKGROUNDS", (0, first), (-1, -1), [colors.white, BAND]),
    ]
    if header:
        style.append(("BACKGROUND", (0, 0), (-1, 0), ACCENT))
    for c in align_right:
        style.append(("ALIGN", (c, 1), (c, -1), "RIGHT"))
    t.setStyle(TableStyle(style))
    story.append(t)
    SP(9)


def callout(title, text, color=WARN, bg=WARNBG):
    inner = [[Paragraph("<b>%s</b>  %s" % (title, text), S["callout"])]]
    t = Table(inner, colWidths=[CW])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, -1), bg),
        ("LINEBEFORE", (0, 0), (0, -1), 2.6, color),
        ("LEFTPADDING", (0, 0), (-1, -1), 9), ("RIGHTPADDING", (0, 0), (-1, -1), 9),
        ("TOPPADDING", (0, 0), (-1, -1), 7), ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
    ]))
    story.append(KeepTogether(t))
    SP(9)


# ============================================================ COVER
story.append(Spacer(1, 42 * mm))
P("Prism Core", "title")
P("Engine Guide &mdash; how a raw event becomes adaptive audio,<br/>what you must know before changing it, "
  "and how to embed it in your software", "subtitle")
story.append(Table([[""]], colWidths=[CW], rowHeights=[2],
                   style=TableStyle([("BACKGROUND", (0, 0), (-1, -1), ACCENT)])))
SP(14)
table([], [
    ["Repository", "prism-core (private)"],
    ["Documents the code at", "commit <font face='DJM'>2f55a27</font>, branch <font face='DJM'>claude/repo-context-overview-8xbj1j</font>"],
    ["Public surface described", "C ABI <font face='DJM'>include/prism/prism_core.h</font>, version 0.3.0 (20 exported functions)"],
    ["Audience", "Engineers integrating, extending or reviewing the engine"],
    ["Status of the numbers", "Every value in Part II was produced by running this commit's code, not by re-deriving formulas"],
    ["Date", "19 September 2026"],
], [38 * mm, CW - 38 * mm], header=False)
SP(10)
P("<b>Scope.</b> This guide covers the engine that exists today: the Prism Context Engine (PCE), the Prism "
  "State Vector (PSV) contract, and the Prism Generative Audio Engine (PGAE), behind one C ABI. The proposed "
  "haptics engine (PGHE) is specified separately in <font face='DJM'>docs/prism-pghe-consumption-spec.md</font> "
  "and is <b>not</b> implemented; it is out of scope here except where the architecture anticipates it.")
P("<b>Confidentiality.</b> This repository is private and nothing in it is disclosed until the patent position "
  "allows. Treat this document the same way.", "small")

story.append(PageBreak())

# ============================================================ CONTENTS
H1("Contents")
table(["Part", "What it answers", "Page topic"], [
    ["Part I", "What goes in, what comes out, and every step between", "The data flow, stage by stage"],
    ["Part II", "How the output changes when the input changes", "Measured transfer tables from the real engine"],
    ["Part III", "What must I know before I change anything?", "Invariants, constant map, tripwires, safe-change recipe"],
    ["Part IV", "How do I attach this as a module to my software?", "ABI, lifecycle, threading, inputs, outputs, code"],
    ["Appendix A", "Every ABI function at a glance", "Function reference"],
    ["Appendix B", "Every tunable constant and where it lives", "Constant reference"],
    ["Appendix C", "Terms", "Glossary"],
], [22 * mm, 82 * mm, CW - 104 * mm])

H2("The engine in one paragraph")
P("Prism Core turns cheap, privacy-safe signals about what a person is doing into continuously adapting "
  "ambient audio. Three kinds of raw event go in (window switches, idle durations, task deadlines) plus the "
  "clock. An inference engine fuses them into a four-dimensional <b>Prism State Vector</b> &mdash; arousal, "
  "valence, cognitive load, readiness &mdash; where every dimension carries its own confidence. A separate "
  "audio engine consumes only that vector and renders a continuous soundscape from pre-loaded loops. Nothing "
  "leaves the device, and the two engines cannot see each other: the state vector is the only thing that "
  "crosses between them.")

H2("The whole pipeline on one line")
code("""
host events  ->  PCE  ->  [ Prism State Vector ]  ->  PGAE  ->  float32 audio
(switch/idle/     (3 adapters      (4 dims, each     (mapping      (mono, your
 deadlines,        + fusion)        value+confidence)  + DSP)       sample rate)
 + clock)              |                   |
                       |                   +-> prism_get_psv  (read-out for your UI)
                       +-- emits every 30 s, or sooner on a >= 0.1 change
""")

callout("The one structural rule.",
        "The PCE emits a state vector and nothing else &mdash; never an audio parameter. The PGAE reads a state "
        "vector and nothing else &mdash; never an input event. This 'firewall' is enforced by the build graph and "
        "by a test, not by convention. Everything else in this document follows from it.",
        ACCENT, BAND)

story.append(PageBreak())

# ============================================================ PART I
H1("Part I &mdash; From input to output, stage by stage")

P("There are eight stages. Stages 1&ndash;4 run on an inference thread the library owns. Stages 6&ndash;8 run on "
  "an audio thread you own (or one the library opens for you). Stage 5 is the handoff between them, and it is "
  "the only place the two halves touch.")

H2("Stage 0 &mdash; What you actually feed in")

P("The engine accepts three event types and a clock. That is the entire input surface. Note what is "
  "<i>not</i> there: no app names, no window titles, no keystrokes, no screenshots, no microphone, no camera, "
  "no network. A window switch is a bare timestamp; idle is a duration; a task is a due instant plus a "
  "priority. App identity is discarded in your shell before it reaches the ABI.")

table(["Input", "ABI call", "What it means", "Who calls it"], [
    ["Window / app switch",
     "prism_report_app_switch(core, t_ms)",
     "The user moved from one context to another at this instant. Frequency of these drives the "
     "<b>scatter</b> signal.",
     "Your shell, on every focus change"],
    ["Idle sample",
     "prism_report_idle(core, t_ms, idle_ms)",
     "As of this instant, the user has been idle for <font face='DJM'>idle_ms</font>. This is a "
     "<b>heartbeat</b>: send one on a fixed timer, not only when idle.",
     "Your shell, every ~5 s"],
    ["Task deadlines",
     "prism_report_task_deadlines(core, tasks, count)",
     "The complete current task list as due instants plus priorities (0 low, 1 medium, 2 high). Replaces "
     "the previous snapshot entirely.",
     "Your shell, whenever the list changes"],
    ["Clock + timezone",
     "prism_config.tz_offset_min",
     "Supplied once at creation (UTC minus local, in minutes). Drives the circadian prior; the engine "
     "reads the system clock itself.",
     "Set at prism_create"],
], [30 * mm, 44 * mm, 62 * mm, CW - 136 * mm], mono_cols=(1,))

callout("The idle heartbeat is load-bearing.",
        "Behavioural confidence is derived from how many idle samples arrived in the last two minutes, not "
        "from their content. If you only report idle when the user goes away, coverage stays near zero, "
        "behavioural confidence stays near zero, and the behavioural half of the vector is inert. Send the "
        "heartbeat on a timer.")

H2("Stage 1 &mdash; Ingress and the rolling window")

P("Events land in memory and nowhere else. Switch timestamps and idle samples go into a rolling "
  "<b>120-second window</b>; anything older than <font face='DJM'>now - 120 s</font> is evicted on the next "
  "read. The task list is a snapshot that replaces its predecessor. Nothing is written to disk, ever &mdash; "
  "that is a privacy invariant of the repository, not a configuration choice.")

P("Ingress calls are thread-safe and cheap. They take a lock shared with the inference thread and append to a "
  "vector; they never block on I/O. You may call them from any thread except an audio callback.")

H2("Stage 2 &mdash; Three adapters reduce the raw signals")

P("Each adapter reduces its signal to a <b>value in [0,1] plus a confidence in [0,1]</b>. An adapter knows "
  "nothing about the state vector and nothing about audio. All three are pure functions of their inputs and "
  "the clock.")

H3("2a. Deadline proximity &mdash; pce/src/deadline.cpp")
code("""
pressure = max over tasks of:  priority_weight x exp(-max(0, due_ms - now) / tau)
tau = 48 h        weights: low 0.4, medium 0.7, high 1.0
confidence = 0.9 if the task list is non-empty, else 0.0 (value falls back to 0.5)
""")
P("The single most pressing task drives the signal; overdue tasks clamp at maximum proximity. Confidence "
  "reflects <i>availability</i>, not freshness &mdash; a due date is a fact, so it is trusted at 0.9 as soon as "
  "it exists, and not at all when the list is empty. Measured, for a high-priority task:")
table(["Due in", "0 h", "2 h", "12 h", "24 h", "48 h", "168 h (1 wk)"],
      [["pressure", "1.000", "0.959", "0.779", "0.607", "0.368", "0.030"]],
      [26 * mm] + [(CW - 26 * mm) / 6.0] * 6, mono_cols=(1, 2, 3, 4, 5, 6))

H3("2b. Attention / behaviour &mdash; pce/src/attention.cpp")
code("""
switches_per_min = switches in window / 2          (window is 120 s)
scatter   = 1 - exp(-switches_per_min / 6)          0 = settled, 1 = fragmented
activity  = 1 - (idle samples over 15 s) / (all idle samples in window)
coverage  = min(1, idle samples / 24)               24 = a full window at 5 s spacing
confidence = coverage x 0.7
""")
P("Two distinct drivers come out: <b>activity</b> (present versus away) and <b>scatter</b> (settled versus "
  "context-switching). They push different dimensions, which is why they are kept separate. The 0.7 ceiling "
  "exists because behaviour is a noisier proxy than a calendar date, so even a perfectly covered window never "
  "reaches full confidence.")

H3("2c. Circadian phase &mdash; pce/src/circadian.cpp")
code("""
daily      = 0.5 + 0.4 x cos(2 pi (h - 16) / 24)           h = local hour, fractional
post_lunch = 0.1 x exp(-(h - 14)^2 / (2 x 1.5^2))
alertness  = clamp01(daily - post_lunch)                   confidence = 0.5, always
""")
P("A 24-hour cosine peaking at 16:00 and troughing near 04:00, minus a small post-lunch dip centred on 14:00. "
  "It is a population-level prior rather than a measurement of this person, so it ships at a fixed middling "
  "confidence of 0.5. Measured across the day:")
table(["Local hour", "00", "04", "08", "10", "12", "14", "16", "18", "20", "22"],
      [["alertness", "0.300", "0.100", "0.300", "0.497", "0.659", "0.746", "0.859", "0.844",
        "0.700", "0.500"]],
      [24 * mm] + [(CW - 24 * mm) / 10.0] * 10, mono_cols=tuple(range(1, 11)), font_size=7.6)
P("Note the dip: 14:00 (0.746) sits below the smooth curve, and the peak is at 16:00, not midday.", "small")

H2("Stage 3 &mdash; Fusion into the state vector")

P("Fusion is one small function, <font face='DJM'>pce/src/fusion.cpp</font>. Each PSV dimension is a "
  "confidence-weighted blend of several <i>contributions</i>. A contribution is a target value, the confidence "
  "in it, and an <b>importance</b> &mdash; a fixed weight saying how much that signal should matter for that "
  "dimension.")

code("""
value      = sum(confidence x importance x target) / sum(confidence x importance)
confidence = sum(confidence x importance) / sum(importance)
if no contribution has any confidence:  value = 0.5, confidence = 0.0
""")

P("Read the second line carefully, because it is the subtle one: a dimension's confidence is the fraction of "
  "its <i>expected</i> input that is actually present and trusted. If a dimension draws on three signals and "
  "only one is available, its confidence is low even if that one signal is certain.")

table(["Dimension", "Contributions (target &larr; signal, importance)", "Meaning"], [
    ["arousal",
     "deadline pressure (1.0) &nbsp;&bull;&nbsp; circadian alertness (0.7) &nbsp;&bull;&nbsp; "
     "behavioural arousal (0.7)<br/>where behavioural arousal = activity &times; (0.55 + 0.45 &times; scatter)",
     "Activation. Deadlines key you up, circadian sets the daily baseline, behaviour adds the moment. "
     "Being away suppresses it regardless of scatter."],
    ["cognitive_load",
     "deadline pressure (1.0) &nbsp;&bull;&nbsp; scatter (0.9)",
     "Pressure and fragmentation both raise load. Deliberate property: high pressure with low scatter lands "
     "at <i>moderate</i> load (flow), while scattered-under-pressure lands high."],
    ["readiness",
     "circadian alertness (1.0) &nbsp;&bull;&nbsp; (1 - deadline pressure) (0.4)",
     "Capacity. The daily curve, eroded by sustained pressure."],
    ["valence",
     "none &mdash; hard-coded {0.5, 0.0}",
     "Reserved slot. Inert in v1: it is emitted, but at zero confidence, so no consumer acts on it."],
], [26 * mm, 84 * mm, CW - 110 * mm])

H2("Stage 4 &mdash; When a vector is emitted")

P("An inference tick runs every <b>5 seconds</b> by default. It does not emit every tick. A vector is "
  "published when either condition holds:")
BL([
    "the nominal cadence has elapsed &mdash; <b>30 s</b> by default for the Aqademiq profile, or",
    "any dimension's value moved by at least <b>0.1</b> since the last emission (the 'significant change' rule).",
])
P("Both numbers are configurable at creation (<font face='DJM'>cadence_ms</font>, "
  "<font face='DJM'>check_interval_ms</font>, <font face='DJM'>significant_delta</font>). Every emission "
  "carries a strictly monotonic <font face='DJM'>sequence</font>, safe to deduplicate on, and an "
  "<font face='DJM'>update_timestamp_ms</font>. The very first vector after "
  "<font face='DJM'>prism_start</font> is the <b>neutral vector</b>: every value 0.5, every confidence 0.0.")

H2("Stage 5 &mdash; The handoff to the audio thread")

P("The vector crosses from the inference thread to the audio thread through a pre-allocated, wait-free "
  "<b>triple buffer</b> (<font face='DJM'>psv/include/psv/exchange.h</font>). One writer, one reader, no mutex, "
  "no allocation, no tearing: the reader always gets the newest complete snapshot or nothing. The audio thread "
  "polls it exactly once per callback.")

P("What crosses is <font face='DJM'>RtStateVector</font> &mdash; a 112-byte trivially copyable mirror of the "
  "vector with fixed arrays instead of strings, because copying a <font face='DJM'>std::string</font> can "
  "allocate and allocation is forbidden on the audio thread.")

H2("Stage 6 &mdash; Confidence weighting (the consumer's first act)")

P("Before any mapping happens, every dimension is collapsed to a single <b>effective value</b> that folds in "
  "its own confidence:")
code("""
effective(d) = 0.5 + (value(d) - 0.5) x confidence(d)
""")
P("At confidence 0 the dimension is exactly neutral no matter how extreme its value; at confidence 1 it passes "
  "through untouched. This is per-dimension and strictly linear &mdash; there is no threshold anywhere. It is "
  "the mechanism that makes an uncertain estimate quietly stop steering the output instead of steering it "
  "wrongly, and it is the single most important line in the engine.")

H2("Stage 7 &mdash; The mapping law: state to audio parameters")

P("One pure function, <font face='DJM'>pgae/src/mapping.cpp</font>, is the only place a state vector becomes "
  "audio intent. It takes the three active effective values, centres them on neutral, and computes every audio "
  "parameter as a saturated affine function of those deviations.")

code("""
a = effective(arousal) - 0.5      l = effective(cognitive_load) - 0.5
r = effective(readiness) - 0.5    (valence has no term in v1)

brightness = clamp01(0.55 + 0.9a - 0.8l)      cutoff_hz = 300 x 40^brightness   (300 Hz .. 12 kHz)
density    = clamp01(0.5  + 0.9a - 1.0l)

gain[bed]   = clamp01(0.6 + 0.6l)     steady bed rises under load - something calm to hold
gain[sub]   = clamp01(0.5 - 0.6r)     low grounding rises as readiness falls
gain[pulse] = clamp01(0.5 + 0.7a - 0.6l)
gain[lead]  = clamp01(0.5 + 0.4a - 0.9l)      melodic foreground recedes hardest under load
gain[air]   = clamp01(0.5 + 0.5a - 0.6l)

active[bed] = active[sub] = always on
active[pulse] = density >= 0.35    active[air] = density >= 0.55    active[lead] = density >= 0.72
""")

P("Two things are worth internalising here. First, <b>load is the dominant term</b> and it points the opposite "
  "way from arousal on nearly every parameter: as cognitive load rises the filter closes, layers drop out, and "
  "the melodic foreground recedes fastest. That is the focus-protection principle, expressed as coefficient "
  "signs rather than as a rule. Second, the <b>density gates</b> are thresholds on a mapped parameter, never on "
  "confidence &mdash; the confidence path stays linear and threshold-free.")

H2("Stage 8 &mdash; The render path")

P("The mapped parameters are targets, not values. Everything the listener hears is smoothed, scheduled and "
  "rail-bounded on the way out. Per sample, in order:")

NL([
    "<b>Smoothed cutoff.</b> A one-pole approach to the target with a 0.6 s time constant; the biquad "
    "coefficients refresh every 32 samples, but the parameter itself moves every sample so it can never step.",
    "<b>Per-stem render.</b> Each stem reads its loop at <font face='DJM'>phase % loop_frames</font> "
    "(sample-accurate), multiplied by its density envelope and its smoothed level (0.25 s time constant). "
    "Level in [0,1] becomes amplitude as <font face='DJM'>0.4 &times; g^1.5</font>.",
    "<b>Density crossfades.</b> When a gate flips, the change is <i>scheduled at that stem's next loop "
    "boundary</i> and applied as a 1.5 s equal-power fade &mdash; never applied immediately, never stepped. A "
    "re-scheduled fade ramps from the envelope's current value, so rapid flips cannot click.",
    "<b>Scene crossfade (optional).</b> If a scene swap is in flight, two decks render and sum under "
    "equal-power gains. This is the only place two scenes exist at once; the outgoing scene is handed back to "
    "the control thread for release, never freed on the audio thread.",
    "<b>Master low-pass</b> at the smoothed cutoff, then a master gain of 0.7 with a 0.5 s linear fade-in from "
    "exact silence at start.",
    "<b>Limiter, last and unbypassable.</b> Peak limiter at -3 dBFS (3 ms attack, 100 ms release) plus a hard "
    "clamp behind it. Nothing upstream can exceed the ceiling.",
])

callout("Real-time rules, in one sentence.",
        "The render path never allocates, never locks, never logs, never blocks and never touches a file. "
        "Everything it reads was allocated when the scene loaded. Two automated checks enforce this: an "
        "instrumented-allocator test over a ten-minute render, and a static audit of the compiler-computed "
        "include closure of the render path for any locking, blocking, logging or allocator symbol.")

H2("What comes out")

table(["Output", "How you get it", "Shape"], [
    ["Adaptive audio",
     "<font face='DJM'>prism_render(core, float*, frames)</font> from your audio callback, "
     "<i>or</i> let the library open a device with <font face='DJM'>prism_device_start</font>",
     "Mono <font face='DJM'>float32</font>, at the scene's sample rate "
     "(<font face='DJM'>prism_sample_rate</font>)"],
    ["Current state",
     "<font face='DJM'>prism_get_psv(core, &amp;out)</font> from any thread, poll it on a UI timer",
     "4 values + 4 confidences, timestamp, sequence, mode hint"],
], [30 * mm, 78 * mm, CW - 108 * mm])

H2("Part I on one page")

table(["#", "Stage", "Where it lives", "Thread", "What leaves it"], [
    ["0", "Event ingress", "prism_core.cpp (ABI)", "any", "Timestamps, durations, due instants"],
    ["1", "Rolling 120 s window", "pce/src/attention.cpp", "inference", "The events of the last two minutes"],
    ["2", "Three adapters", "pce/src/{deadline,attention,circadian}.cpp", "inference",
     "value + confidence, per signal"],
    ["3", "Fusion", "pce/src/fusion.cpp", "inference", "A Prism State Vector (4 x value+confidence)"],
    ["4", "Emission policy", "pce/src/pce.cpp", "inference", "A vector, every 30 s or on a 0.1 change"],
    ["5", "Wait-free handoff", "psv/include/psv/exchange.h", "crosses", "A 112-byte snapshot, no locks"],
    ["6", "Confidence weighting", "psv/include/psv/psv.h", "audio", "One effective value per dimension"],
    ["7", "Mapping law", "pgae/src/mapping.cpp", "audio", "Cutoff, density, 5 gains, 5 gates"],
    ["8", "Render + rails", "pgae/src/engine.cpp", "audio", "float32 samples, limiter-bounded"],
], [8 * mm, 32 * mm, 52 * mm, 17 * mm, CW - 109 * mm], font_size=7.7, mono_cols=(2,))

P("Read down the last column and the firewall is visible as a shape: raw events never travel past stage 3, "
  "and audio parameters never exist before stage 7. The only thing that crosses the middle is the vector.")

story.append(PageBreak())

# ============================================================ PART II
H1("Part II &mdash; How the output changes with the input")

P("Every number in this part was produced by compiling this commit and running scripted inputs through the "
  "real PCE and the real mapping function. Nothing here is a re-derivation by hand. Reading guide: "
  "<font face='DJM'>a/l/r</font> are arousal, cognitive load and readiness as "
  "<font face='DJM'>value/confidence</font>; <font face='DJM'>eff</font> is the confidence-weighted effective "
  "value that actually drives audio; <font face='DJM'>cut</font> is the master low-pass cutoff; "
  "<font face='DJM'>den</font> is density; layers listed are those above their gate.")

H2("A. Nine end-to-end scenarios")

table(["Scenario", "arousal", "load", "readi&shy;ness", "cutoff Hz", "density", "layers on"], [
    ["Cold start (no input yet)", "0.50/0.00", "0.50/0.00", "0.50/0.00", "2282", "0.500",
     "bed, sub, pulse"],
    ["Deep focus, deadline 24 h", "0.57/0.72", "0.36/0.81", "0.45/0.61", "3783", "0.660",
     "bed, sub, pulse, air"],
    ["Deep focus, deadline 2 h", "0.75/0.72", "0.56/0.81", "0.31/0.61", "3584", "0.612",
     "bed, sub, pulse, air"],
    ["Scattered, deadline 2 h", "0.92/0.72", "0.92/0.81", "0.49/0.61", "2325", "0.437",
     "bed, sub, pulse"],
    ["Scattered, no deadline", "0.88/0.35", "0.87/0.33", "0.81/0.36", "2494", "0.500",
     "bed, sub, pulse"],
    ["Away (idle 20 s)", "0.48/0.72", "0.36/0.81", "0.63/0.61", "3026", "0.600",
     "bed, sub, pulse, air"],
    ["Night wind-down (23:00)", "0.30/0.72", "0.12/0.81", "0.61/0.61", "3516", "0.679",
     "bed, sub, pulse, air"],
    ["Fresh morning, no tasks", "0.53/0.35", "0.15/0.33", "0.40/0.36", "3301", "0.623",
     "bed, sub, pulse, air"],
    ["Post-lunch dip (14:00)", "0.56/0.72", "0.37/0.81", "0.68/0.61", "3626", "0.647",
     "bed, sub, pulse, air"],
], [40 * mm, 21 * mm, 21 * mm, 21 * mm, 18 * mm, 16 * mm, CW - 137 * mm], font_size=7.7,
    mono_cols=(1, 2, 3, 4, 5))

P("<b>The headline comparison is rows 3 and 4.</b> Same deadline two hours out, same time of day; the only "
  "difference is that the user is switching windows constantly. Load goes from 0.56 to 0.92, the filter closes "
  "from 3584 Hz to 2325 Hz, density falls below the <font face='DJM'>air</font> gate, and the airy layer drops "
  "out. Under pressure <i>and</i> fragmentation the soundscape gets darker and simpler &mdash; it recedes "
  "rather than energising. That is the entire product thesis, visible in four numbers.")

H2("B. One input at a time")

H3("B1. Deadline distance &mdash; high priority, 10:00, settled, full behavioural coverage")
table(["Due in", "arousal", "load", "readiness", "cutoff Hz", "density", "layers on"], [
    ["1 h", "0.761", "0.576", "0.298", "3573", "0.609", "bed sub pulse air"],
    ["6 h", "0.711", "0.519", "0.338", "3626", "0.623", "bed sub pulse air"],
    ["12 h", "0.658", "0.458", "0.382", "3684", "0.637", "bed sub pulse air"],
    ["24 h", "0.569", "0.357", "0.454", "3783", "0.660", "bed sub pulse air"],
    ["48 h", "0.445", "0.216", "0.554", "3923", "0.693", "bed sub pulse air"],
    ["96 h", "0.325", "0.080", "0.651", "4065", "0.724", "bed sub pulse <b>lead</b> air"],
    ["1 week", "0.271", "0.018", "0.695", "4131", "0.739", "bed sub pulse <b>lead</b> air"],
], [20 * mm, 21 * mm, 19 * mm, 23 * mm, 20 * mm, 19 * mm, CW - 122 * mm], font_size=7.9,
    mono_cols=(1, 2, 3, 4, 5))
P("Monotone in both directions: pressure raises arousal and load, and lowers readiness. Watch the "
  "<font face='DJM'>lead</font> layer &mdash; the melodic foreground &mdash; appear only once density crosses "
  "0.72, which happens when the deadline is four days out. Melody is a reward for having room to think.")

H3("B2. Window-switch rate &mdash; deadline 24 h high, 10:00")
table(["Switches per 2 min", "arousal", "load", "readiness", "cutoff Hz", "density", "layers on"], [
    ["0", "0.569", "0.357", "0.454", "3783", "0.660", "bed sub pulse air"],
    ["2", "0.588", "0.420", "0.454", "3411", "0.622", "bed sub pulse air"],
    ["6", "0.618", "0.519", "0.454", "2902", "0.562", "bed sub pulse air"],
    ["12", "0.649", "0.617", "0.454", "2471", "0.503", "bed sub pulse"],
    ["24", "0.678", "0.713", "0.454", "2113", "0.445", "bed sub pulse"],
    ["48", "0.693", "0.761", "0.454", "1953", "0.416", "bed sub pulse"],
], [30 * mm, 20 * mm, 19 * mm, 22 * mm, 20 * mm, 19 * mm, CW - 130 * mm], font_size=7.9,
    mono_cols=(1, 2, 3, 4, 5))
P("Scatter saturates: the step from 0 to 6 switches costs more brightness than the step from 24 to 48, because "
  "<font face='DJM'>scatter = 1 - exp(-rate/6)</font> flattens out. Readiness does not move at all &mdash; it "
  "has no behavioural contribution.")

H3("B3. Time of day &mdash; deadline 24 h high, settled")
table(["Local hour", "arousal", "load", "readiness", "cutoff Hz", "density"], [
    ["04:00", "0.489", "0.357", "0.223", "3121", "0.608"],
    ["09:00", "0.548", "0.357", "0.395", "3602", "0.647"],
    ["14:00", "0.619", "0.357", "0.599", "4268", "0.693"],
    ["16:00", "0.641", "0.357", "0.664", "4507", "0.708"],
    ["20:00", "0.609", "0.357", "0.572", "4173", "0.687"],
    ["23:00", "0.548", "0.357", "0.395", "3603", "0.647"],
], [26 * mm, 24 * mm, 24 * mm, 26 * mm, 26 * mm, CW - 126 * mm], font_size=7.9,
    mono_cols=(1, 2, 3, 4, 5))
P("Load is flat because the circadian prior contributes nothing to it. Note that 09:00 and 23:00 are nearly "
  "identical &mdash; the curve is a cosine, so morning and late evening are symmetric about the 16:00 peak. If "
  "you want them to feel different, the circadian adapter is where you would change it, not the mapping.")

H3("B4. Idle duration reported on each heartbeat")
table(["Idle per sample", "arousal", "load", "cutoff Hz", "density", "Effect"], [
    ["0 ms", "0.569", "0.357", "3783", "0.660", "counts as active"],
    ["10 000 ms", "0.569", "0.357", "3783", "0.660", "still active &mdash; below threshold"],
    ["16 000 ms", "0.414", "0.357", "2605", "0.559", "counts as away"],
    ["30 000 ms", "0.414", "0.357", "2605", "0.559", "identical to 16 s"],
], [28 * mm, 22 * mm, 22 * mm, 22 * mm, 22 * mm, CW - 116 * mm], font_size=7.9,
    mono_cols=(1, 2, 3, 4))
P("This one is a <b>step, not a ramp</b>. The 15-second threshold makes a sample either 'present' or 'away'; "
  "10 s and 0 s are indistinguishable, and so are 16 s and 30 s. Only the <i>fraction</i> of away-samples in "
  "the window varies continuously. Integrators often expect idle to be proportional. It is not.")

H2("C. Confidence as an independent lever")

P("Same extreme vector (arousal 0.9, load 0.9, readiness 0.2), confidence swept from 0 to 1:")
table(["Confidence", "eff arousal", "eff load", "eff readiness", "cutoff Hz", "density", "bed gain"], [
    ["0.00", "0.500", "0.500", "0.500", "2282", "0.500", "0.600"],
    ["0.25", "0.600", "0.600", "0.425", "2367", "0.490", "0.660"],
    ["0.50", "0.700", "0.700", "0.350", "2456", "0.480", "0.720"],
    ["0.75", "0.800", "0.800", "0.275", "2549", "0.470", "0.780"],
    ["1.00", "0.900", "0.900", "0.200", "2645", "0.460", "0.840"],
], [22 * mm, 24 * mm, 20 * mm, 26 * mm, 22 * mm, 20 * mm, CW - 134 * mm], font_size=7.9,
    mono_cols=(1, 2, 3, 4, 5, 6))
P("Every effective value moves exactly linearly with confidence, and at zero confidence the engine sits at "
  "precisely its neutral operating point (2282 Hz, density 0.5) regardless of how extreme the underlying "
  "estimate was. When you see the engine 'not responding', check confidence before you check values &mdash; a "
  "shell that never sends the idle heartbeat produces exactly this.")

H2("Five behaviours that surprise people")

BL([
    "<b>Low load opens the filter as much as high arousal does.</b> Night wind-down (arousal 0.30) is "
    "<i>brighter</i> than cold start, because load is near zero and the <font face='DJM'>-0.8l</font> term "
    "dominates. Brightness is not a proxy for energy.",
    "<b>Confidence and value are independent axes.</b> 'Scattered, no deadline' has values nearly as extreme as "
    "'scattered, deadline 2 h', but every confidence is roughly half, so the audio barely moves. An empty task "
    "list does not just remove deadline pressure &mdash; it lowers confidence on all three dimensions.",
    "<b>Arousal and load usually move together, then diverge.</b> Both rise with deadline pressure; only load "
    "responds to fragmentation. Deep pressure with no switching is the 'flow' corner: high arousal, moderate "
    "load, bright and dense.",
    "<b>Layers appear and disappear at loop boundaries, not instantly.</b> A gate crossing schedules a 1.5 s "
    "equal-power fade at that stem's next loop point, so the audible change can trail the state change by "
    "several seconds. This is deliberate; do not treat it as latency.",
    "<b>Valence does nothing.</b> It is emitted at confidence 0 and has no coefficient in the mapping. Reading "
    "it in your UI is fine; acting on it is not.",
])

story.append(PageBreak())

# ============================================================ PART III
H1("Part III &mdash; Changing the engine safely")

P("This part assumes you are about to edit the repository. Read the four invariants first: they are the "
  "constraints that automated checks enforce, and breaking one fails the build or the test suite rather than "
  "producing a subtly wrong engine.")

H2("The four invariants")

table(["Invariant", "What it means in practice", "How it is enforced"], [
    ["<b>The firewall</b>",
     "<font face='DJM'>psv/</font> depends on nothing. <font face='DJM'>pce/</font> and "
     "<font face='DJM'>pgae/</font> each depend on <font face='DJM'>psv/</font> and nothing else. "
     "<font face='DJM'>pgae/</font> must never see <font face='DJM'>pce/</font>, or the reverse. Only "
     "<font face='DJM'>core/</font> links both.",
     "CMake target graph makes the headers unreachable; the "
     "<font face='DJM'>firewall_includes</font> ctest additionally greps for relative-path and "
     "macro-indirected includes that would evade it."],
    ["<b>Real-time safety</b>",
     "No allocation, lock, log, blocking call or file I/O anywhere the audio thread can reach. Everything "
     "pre-allocated at scene load. Every gain and routing change ramped; nothing steps.",
     "<font face='DJM'>PgaeRtSafety</font> runs an instrumented global allocator over a ten-minute render; "
     "<font face='DJM'>rt_lock_primitives</font> statically audits the compiler-computed include closure of "
     "the render path."],
    ["<b>Probe parity</b>",
     "The PCE reproduces the validated TypeScript probe to 1e-6 per dimension per tick. The heuristic "
     "constants are dogfooding findings, not guesses &mdash; they are ported verbatim and must not be "
     "'improved' in this slice.",
     "<font face='DJM'>tests/golden/fixture-*.jsonl</font> replayed by the parity test; divergence fails."],
    ["<b>Privacy</b>",
     "No network code anywhere in the core. Raw events are processed in memory and never written to disk. "
     "Only derived values may be logged, and only when the host opts in.",
     "By construction &mdash; there is no networking library in the dependency set, and adding one is a "
     "stop-and-ask item."],
], [26 * mm, 74 * mm, CW - 100 * mm], font_size=8.0)

H2("Where every knob lives")

P("If you want to change how the engine behaves, this table tells you which file to open and what you are "
  "risking. The right-hand column is the honest cost.")

table(["I want to change...", "File", "What it affects", "What it costs"], [
    ["How fast deadline pressure ramps",
     "pce/include/pce/deadline.h",
     "tau (48 h), priority weights, base confidence 0.9",
     "Breaks golden-trace parity. Requires re-recording fixtures and a documented decision."],
    ["How switching maps to scatter",
     "pce/include/pce/attention.h",
     "window 120 s, heartbeat 5 s, idle threshold 15 s, scale 6/min, confidence ceiling 0.7",
     "Breaks parity. Also changes confidence, so it moves the whole vector, not just load."],
    ["The daily alertness curve",
     "pce/src/circadian.cpp",
     "peak hour, amplitude, post-lunch dip, fixed confidence 0.5",
     "Breaks parity. This is the right place to fix morning/evening symmetry."],
    ["How signals combine into dimensions",
     "pce/src/fusion.cpp",
     "importances, the behavioural-arousal expression, which signals feed which dimension",
     "Breaks parity, and changes confidence semantics. The highest-leverage and highest-risk file in the PCE."],
    ["How state becomes sound",
     "pgae/src/mapping.cpp",
     "brightness/density coefficients, per-stem gains, gate thresholds 0.35/0.55/0.72",
     "Does not break PCE parity, but changes the validated feel. Re-run the render tests and dogfood."],
    ["Smoothing, crossfade, limiter",
     "pgae/include/pgae/engine.h (PgaeOptions)",
     "tc_gain 0.25 s, tc_cutoff 0.6 s, crossfade 1.5 s, amp_trim 0.4, master 0.7, limiter -3 dBFS",
     "Risk of audible steps or clipping. The no-step and ceiling tests are your tripwire."],
    ["Emit cadence / responsiveness",
     "prism_config at runtime",
     "cadence_ms, check_interval_ms, significant_delta",
     "Safe &mdash; these are host configuration, not engine constants. Start here."],
    ["The musical material",
     "assets/scenes.json + stems",
     "which loops make up a scene, their key and length",
     "Safe for the engine; loop lengths should stay coprime so the composite does not repeat audibly."],
], [34 * mm, 34 * mm, 48 * mm, CW - 116 * mm], font_size=7.7, mono_cols=(1,))

callout("The cheapest change is almost always the manifest or the config.",
        "Before touching a coefficient, ask whether a different scene, a different cadence, or a host-side "
        "mood override gets you there. Those paths carry no parity risk and no review burden.", ACCENT, BAND)

H2("The test suite is the contract")

P("Run <font face='DJM'>ctest --preset debug</font> before and after any change. What each guard catches:")

table(["Check", "Catches"], [
    ["<font face='DJM'>firewall_includes</font>", "Any cross-module include that would collapse the "
     "architecture, including relative-path and macro-indirected attempts."],
    ["<font face='DJM'>rt_lock_primitives</font>", "A mutex, logger, allocator or blocking call appearing "
     "anywhere in the render path's include closure &mdash; even indirectly, via a header you did not think "
     "about."],
    ["<font face='DJM'>PgaeRtSafety</font>", "Any allocation on the audio thread during a ten-minute render "
     "with live state publishing."],
    ["<font face='DJM'>PgaeRender.*</font>", "Limiter ceiling exceeded; a parameter stepping instead of "
     "gliding; a crossfade not landing exactly on the loop boundary; equal-power property violated."],
    ["<font face='DJM'>PceParity</font> / golden traces", "Any divergence from the validated probe beyond "
     "1e-6 per dimension per tick."],
    ["<font face='DJM'>PsvSchema</font> / JSON schema", "An emitted vector that no longer validates against "
     "the published contract."],
    ["<font face='DJM'>abi_test</font>, <font face='DJM'>c_compat.c</font>", "An ABI change that breaks the "
     "version tripwire or stops compiling as plain C (which would break the Dart bindings)."],
], [42 * mm, CW - 42 * mm], font_size=8.0)

H2("A safe-change recipe")

NL([
    "Reproduce the current behaviour first. Run <font face='DJM'>ctest</font> green, and capture an offline "
    "render (<font face='DJM'>prism_harness --render before.wav --seconds 60</font>) so you have an A/B.",
    "Make the smallest possible change, in one file, and write down <i>why</i> in a comment next to the "
    "constant. Every heuristic constant in this repo carries its rationale; yours must too.",
    "Run the suite. If parity fails, stop and decide deliberately: either revert, or re-record the golden "
    "fixtures and record the divergence as a reviewed decision. Silent divergence is the one unacceptable "
    "outcome.",
    "Listen. The render tests prove the engine is well-behaved, never that it sounds right. Capture an after "
    "render and compare.",
    "Re-measure if you touched anything on the audio path: CPU, resident memory and allocation count, into "
    "<font face='DJM'>docs/perf-desktop.md</font>.",
    "One change per review. A human reviews every task in this repository.",
])

H2("Stop and ask before you do any of these")

BL([
    "Anything that would make <font face='DJM'>pgae/</font> see <font face='DJM'>pce/</font> or the reverse &mdash; "
    "if a change seems to require it, the design is wrong, not the firewall.",
    "Adding a third-party dependency. The stack is deliberately locked to C++17, CMake, miniaudio and "
    "GoogleTest.",
    "Changing the shape of the C ABI &mdash; adding, removing or altering a declaration in "
    "<font face='DJM'>include/prism/prism_core.h</font>. Additions are a minor version and still need sign-off; "
    "changes are a major version.",
    "Anything that relaxes a real-time rule, even temporarily, even behind a flag.",
    "Anything requiring network access, or writing raw input events anywhere.",
    "Adding a module (for example the proposed <font face='DJM'>pghe/</font>).",
])

H2("Known sharp edges in the current code")

P("These were found by review and are not yet fixed. If you are working nearby, know about them.", "small")
BL([
    "<b>Scene ownership across a swap.</b> The retirement handshake between the audio and control threads has "
    "a narrow window in which a stranded scene can be released while still referenced. Read "
    "<font face='DJM'>prism_core.cpp</font> around the crossfade paths carefully before changing them.",
    "<b><font face='DJM'>crossfade_active()</font> reads two atomics</b> and can therefore report a state that "
    "never existed as a single instant. Fine for the UI hint it is used for; not a synchronisation primitive.",
    "<b>Denormal performance on x86.</b> Long decays into near-silence can hit denormal arithmetic and cost "
    "roughly an order of magnitude on some x86 targets. Not observed as a dropout, but it is there.",
    "<b>The asan preset does not link</b> as-is: the instrumented-allocator test lacks the nothrow "
    "<font face='DJM'>new</font>/<font face='DJM'>delete</font> overloads the sanitizer expects.",
    "<b>Stem loop lengths are not coprime</b> in the shipped asset set: bed, sub and air are 8 s, lead 6 s "
    "and pulse 4 s, so the whole composite repeats every 24 s instead of running for minutes without "
    "recurrence. Re-cutting the stems to coprime lengths is an asset change, not a code change.",
])

story.append(PageBreak())

# ============================================================ PART IV
H1("Part IV &mdash; Embedding the engine in your software")

H2("The rule that makes everything else simple")

P("Your software talks to exactly one file: <font face='DJM'>include/prism/prism_core.h</font>. Nothing else "
  "in the repository is public. The desktop harness in this repo is written against that header and nothing "
  "else, deliberately, so it doubles as a worked reference. If you find yourself wanting to include a module "
  "header, stop &mdash; the ABI is missing something and that is a conversation, not a workaround.")

P("The handle is opaque: <font face='DJM'>prism_core*</font>. One handle is one complete engine &mdash; one "
  "inference engine, one audio engine, and the exchange between them. Nothing is global, so multiple handles "
  "in one process are fine.")

H2("What you build and link")

table(["Target", "Artifact", "How you load it"], [
    ["Desktop (Linux/macOS/Windows)", "<font face='DJM'>libprism_core.{a,so,dylib,dll}</font>",
     "Link directly, or <font face='DJM'>dlopen</font>. Set "
     "<font face='DJM'>PRISM_BUILD_SHARED=ON</font> for the loadable form."],
    ["Android", "<font face='DJM'>libprism_core.so</font> per ABI",
     "Bundle in the APK; <font face='DJM'>DynamicLibrary.open</font> from Dart, or JNI from Kotlin."],
    ["iOS", "<font face='DJM'>prism_core.xcframework</font>",
     "Built by <font face='DJM'>tools/build_ios_framework.sh</font>; embedded and signed by the app build."],
    ["Flutter / Dart", "the shared library above",
     "<font face='DJM'>package:prism_core_bindings</font> (ffigen-generated from the header)."],
], [42 * mm, 52 * mm, CW - 94 * mm], font_size=8.0)

P("Only <font face='DJM'>prism_*</font> symbols are exported; everything else is hidden. A shared build "
  "exposes exactly the twenty functions in Appendix A.", "small")

H2("Lifecycle: the call order that is legal")

code("""
prism_create(&config, &core)          // always first
  prism_load_scene(core, "scenes.json")   // decodes every stem HERE. Blocking. Once per handle.
  prism_haptic_... (not implemented)
  prism_start(core)                       // emits the neutral vector, starts the inference thread
    prism_report_* (any thread, any time, repeatedly)
    prism_get_psv  (any thread, poll)
    prism_render   (ONE audio thread)   -- or --   prism_device_start (library owns the device)
    prism_set_mood_override / prism_clear_mood_override
    prism_crossfade_scene / prism_crossfade_active
  prism_stop(core)                        // stops inference + device; idempotent; restartable
prism_destroy(core)                   // only after your audio thread has fully quiesced
""")

table(["Rule", "Consequence if broken"], [
    ["<font face='DJM'>prism_load_scene</font> before <font face='DJM'>prism_start</font>, once per handle",
     "<font face='DJM'>PRISM_ERROR_INVALID_STATE</font>. There is no way to load a second scene; use "
     "<font face='DJM'>prism_crossfade_scene</font> to move between scenes."],
    ["<font face='DJM'>prism_get_psv</font> only after <font face='DJM'>prism_start</font>",
     "<font face='DJM'>PRISM_ERROR_INVALID_STATE</font> &mdash; no vector exists before start."],
    ["Pull model and built-in device are mutually exclusive",
     "Calling <font face='DJM'>prism_render</font> while the device runs renders the same stream twice and "
     "consumes the state twice."],
    ["<b>Quiesce before destroy</b>",
     "Undefined behaviour &mdash; use-after-free. See the callout below; this is the crash integrators "
     "actually hit."],
], [58 * mm, CW - 58 * mm], font_size=8.0)

callout("The one rule that will crash you.",
        "No library can stop a thread it does not own. If your audio thread can be inside "
        "prism_render when prism_destroy runs, you have a use-after-free. Stop your audio callback, join or "
        "otherwise prove the thread has left the library, and only then destroy. The built-in device and the "
        "internal inference thread are owned by the library and are shut down for you.")

H2("Threading contract")

table(["Thread", "May call", "Must not"], [
    ["Your control thread",
     "<font face='DJM'>create</font>, <font face='DJM'>load_scene</font>, <font face='DJM'>start</font>, "
     "<font face='DJM'>stop</font>, <font face='DJM'>destroy</font>, <font face='DJM'>crossfade_scene</font>, "
     "<font face='DJM'>device_*</font>",
     "Call these concurrently with each other &mdash; they are not thread-safe against one another."],
    ["Any thread",
     "<font face='DJM'>prism_report_*</font>, <font face='DJM'>prism_get_psv</font>, "
     "<font face='DJM'>prism_set_mood_override</font>, <font face='DJM'>prism_clear_mood_override</font>, "
     "<font face='DJM'>prism_crossfade_active</font>",
     "Call <font face='DJM'>prism_report_*</font> from the audio callback &mdash; it takes a lock."],
    ["Exactly one audio thread",
     "<font face='DJM'>prism_render</font>",
     "Two threads calling <font face='DJM'>prism_render</font> breaks the single-reader contract on the "
     "state exchange."],
    ["Library-owned (inference, device)",
     "&mdash;",
     "You never see these; <font face='DJM'>prism_destroy</font> shuts them down."],
], [38 * mm, 66 * mm, CW - 104 * mm], font_size=8.0)

H2("How to pass inputs")

P("Map your platform's signals onto the three report calls. Timestamps are epoch milliseconds from the host "
  "clock &mdash; use one consistent clock, and do not mix monotonic and wall-clock values.")

table(["Platform", "Window switch", "Idle heartbeat"], [
    ["macOS", "<font face='DJM'>NSWorkspace</font> active-application notification",
     "<font face='DJM'>CGEventSourceSecondsSinceLastEventType</font> on a 5 s timer"],
    ["Windows", "<font face='DJM'>SetWinEventHook(EVENT_SYSTEM_FOREGROUND)</font>",
     "<font face='DJM'>GetLastInputInfo</font> on a 5 s timer"],
    ["Linux/X11", "<font face='DJM'>_NET_ACTIVE_WINDOW</font> property change",
     "XScreenSaver idle time on a 5 s timer"],
    ["Android", "App/activity lifecycle or usage-stats transitions",
     "Time since last touch/interaction, on a 5 s timer"],
    ["iOS", "App foreground/background transitions (system-limited)",
     "Time since last interaction within your app"],
], [24 * mm, 66 * mm, CW - 90 * mm], font_size=8.0)

P("For task deadlines, send the whole list whenever it changes &mdash; the call replaces the previous "
  "snapshot, so there is no add/remove protocol. An empty list is valid and means 'no deadline pressure, and "
  "low confidence because of it'. The list is capped at 4096 entries.")

callout("Privacy is your responsibility at the boundary, not the engine's.",
        "The ABI cannot receive an app name, so the engine cannot leak one. But your capture layer sees them. "
        "Hash and discard in the shell; forward only the bare timestamp. That is what makes the whole-system "
        "privacy claim true.", ACCENT, BAND)

H2("How to receive output")

H3("Audio, model 1 &mdash; you own the device (recommended for apps)")
code("""
uint32_t rate = prism_sample_rate(core);      // 0 until a scene is loaded
// ... open your own output stream at `rate`, mono float32 ...

// inside your audio callback, on ONE thread:
prism_result r = prism_render(core, out_frames, frame_count);
// r == PRISM_OK and out_frames is filled; on error the buffer is filled with silence.
""")
P("This is what a Flutter app, a game engine or a DAW plugin does. It keeps the device policy &mdash; latency, "
  "routing, ducking, session category &mdash; entirely in your hands, and it is the path the Dart bindings use.")

H3("Audio, model 2 &mdash; the library owns the device (quick starts, embedded)")
code("""
prism_device_start(core);    // opens a default playback device and renders internally
// ... nothing else to do; do NOT also call prism_render ...
prism_device_stop(core);
""")

H3("State read-out, for your UI")
code("""
prism_psv psv;
if (prism_get_psv(core, &psv) == PRISM_OK && psv.sequence != last_seen) {
    last_seen = psv.sequence;
    // psv.arousal, psv.arousal_confidence, ... psv.readiness_confidence
    // psv.update_timestamp_ms, psv.mode_hint (empty string == no hint)
    render_meters(psv);
}
""")
P("Poll on a UI timer &mdash; once a second is plenty, since the engine emits at most every few seconds. "
  "Deduplicate on <font face='DJM'>sequence</font>, which is strictly monotonic across the handle's life "
  "including restarts. Show confidence somewhere: a value without its confidence is misleading, and users "
  "notice when a meter moves confidently on no evidence.")

H3("Steering the engine from the host: mood override")
P("Sometimes a human must overrule inference &mdash; a venue manager picking 'evening warmth', a user choosing "
  "a mode. A mood is <b>a point in state-vector space</b>, not a volume knob:")
code("""
prism_mood_override o = {0};
o.mode_hint = "venue_peak";
o.arousal = 0.85; o.valence = 0.70; o.cognitive_load = 0.30; o.readiness = 0.80;
o.confidence = 1.0;                  // a manual pin is a statement of fact: use 1.0
prism_set_mood_override(core, &o);   // published immediately, then ramped like any change
// ...
prism_clear_mood_override(core);     // inference resumes at the next tick
""")
P("Confidence here is load-bearing for the reason Part II demonstrated: publish a mood at confidence 0.3 and "
  "the engine will pull it most of the way back to neutral and barely change the sound. The override reaches "
  "the audio through the same mapping, smoothing and limiter as an inferred vector &mdash; nothing is bypassed.")

H3("Changing scenes without a gap")
code("""
prism_scene_swap swap = {0};
swap.scenes_json_path = "scenes_evening.json";
swap.crossfade_ms = 4000;               // 0 selects 1500 ms; clamped to [50, 120000]
swap.align_to_loop_boundary = 1;        // musically clean, but costs up to one loop of delay
prism_result r = prism_crossfade_scene(core, &swap);   // BLOCKS to decode; control thread only
if (r == PRISM_ERROR_BUSY) { /* a crossfade is already in flight; retry later */ }
""")
P("The new scene's sample rate must match the loaded one &mdash; there is no resampler. Use "
  "<font face='DJM'>prism_crossfade_active</font> to avoid asking for a swap that would only come back busy.")

H2("Error codes and what to do")

table(["Code", "Means", "Your move"], [
    ["<font face='DJM'>PRISM_OK</font>", "Success", "&mdash;"],
    ["<font face='DJM'>INVALID_ARGUMENT</font>", "Null or malformed parameter; out-of-range value; a swap "
     "whose sample rate differs", "Fix the call. This is a programming error, not a runtime condition."],
    ["<font face='DJM'>INVALID_STATE</font>", "The call violates the lifecycle &mdash; no scene, not started, "
     "already started, second scene load", "Check your ordering against the lifecycle diagram."],
    ["<font face='DJM'>IO</font>", "Scene manifest or a stem file is unreadable or invalid",
     "Check paths: stem paths resolve relative to the manifest, not the working directory."],
    ["<font face='DJM'>DEVICE</font>", "The built-in device could not be opened or started",
     "Fall back to the pull model, or surface a device-selection UI."],
    ["<font face='DJM'>OUT_OF_MEMORY</font>", "Decoding the scene exhausted memory",
     "Smaller or fewer stems. Decoded audio is the dominant allocation."],
    ["<font face='DJM'>BUSY</font>", "A scene crossfade is already in flight",
     "Retry after <font face='DJM'>prism_crossfade_active</font> goes false. Not an error, a state."],
], [36 * mm, 60 * mm, CW - 96 * mm], font_size=8.0)

P("<font face='DJM'>prism_result_description(code)</font> returns a static human-readable string for logs. No "
  "C++ exception ever crosses the ABI; internal failures surface as these codes.", "small")

H2("A complete minimal integration")

code("""
#include "prism/prism_core.h"

prism_core* core = NULL;
prism_config cfg = prism_config_default();
cfg.tz_offset_min = -240;                 // UTC minus local, in minutes (UAE: -240)

if (prism_create(&cfg, &core) != PRISM_OK) return 1;
if (prism_load_scene(core, "assets/scenes.json") != PRISM_OK) { prism_destroy(core); return 1; }

// tell the engine what the user is facing
prism_task_deadline tasks[] = { { now_ms() + 24*3600*1000LL, 2 } };   // due tomorrow, high
prism_report_task_deadlines(core, tasks, 1);

if (prism_start(core) != PRISM_OK) { prism_destroy(core); return 1; }
prism_device_start(core);                 // simplest audio path

// ---- your 5-second heartbeat, on any thread ----
//   prism_report_idle(core, now_ms(), idle_ms_from_os());
//   prism_report_app_switch(core, now_ms());      // only when focus actually changes

// ---- your 1-second UI poll ----
//   prism_psv psv; prism_get_psv(core, &psv);

prism_device_stop(core);
prism_stop(core);
prism_destroy(core);                      // audio thread must be quiesced first
""")

H3("The same thing in Dart")
code("""
final core = PrismCore.create(tzOffsetMin: -240);
core.loadScene('assets/scenes.json');
core.reportTaskDeadlines([TaskDeadline(dueMs: ..., priority: 2)]);
core.start();

Timer.periodic(const Duration(seconds: 5), (_) {
  core.reportIdle(DateTime.now().millisecondsSinceEpoch, idleMs);
});
Timer.periodic(const Duration(seconds: 1), (_) {
  final psv = core.psv;               // null before start
  setState(() => _arousal = psv?.arousal ?? 0.5);
});
""")

H2("Integration checklist")

BL([
    "Idle heartbeat on a fixed timer, roughly every 5 s &mdash; not only when the user goes idle.",
    "App-switch events only on real focus changes, with app identity stripped in your shell.",
    "Full task list re-sent on every change; empty list is legal.",
    "One consistent epoch-millisecond clock for every timestamp.",
    "Exactly one audio path chosen: your callback <i>or</i> the built-in device, never both.",
    "<font face='DJM'>prism_render</font> called from exactly one thread.",
    "Audio thread proven quiesced before <font face='DJM'>prism_destroy</font>.",
    "UI deduplicates on <font face='DJM'>sequence</font> and displays confidence alongside value.",
    "Every return code checked; <font face='DJM'>BUSY</font> handled as a retry, not a failure.",
    "Timezone offset set correctly at creation &mdash; a wrong offset silently shifts the whole circadian curve.",
])

H2("Things integrators get wrong")

BL([
    "<b>Treating the engine as request/response.</b> It is a continuous system. You are not asking a question; "
    "you are keeping a model warm. Sparse input produces low confidence, and low confidence produces neutral "
    "output &mdash; which looks like 'it isn't working'.",
    "<b>Expecting instant audible response.</b> Inference ticks every 5 s and emits every 30 s (or on a 0.1 "
    "change); then gains glide over 0.25 s, the filter over 0.6 s, and layer changes wait for a loop boundary. "
    "Seconds, by design.",
    "<b>Sending idle only when away.</b> The single most common integration bug. See the heartbeat callout.",
    "<b>Re-creating the handle to 'reset'.</b> Use <font face='DJM'>prism_stop</font> then "
    "<font face='DJM'>prism_start</font>; the sequence continues and the scene stays loaded.",
    "<b>Calling <font face='DJM'>prism_report_*</font> from the audio callback.</b> It takes a lock. Use a "
    "queue on your side.",
    "<b>Assuming stereo.</b> Output is mono float32; duplicate to both channels yourself.",
])

H2("Where things live in the repository")

table(["Path", "What is in it", "Touch it if..."], [
    ["include/prism/prism_core.h", "The only public header. The entire integration surface.",
     "You are integrating. Changing it is a stop-and-ask."],
    ["psv/", "The contract: vector types, validation, JSON, the real-time snapshot and the wait-free "
     "exchange. Depends on nothing.", "You are adding a dimension (a major decision) or a transport."],
    ["pce/", "Inference: the three adapters, fusion, the emit policy. Depends on psv only.",
     "You are changing how state is inferred."],
    ["pgae/", "Audio: mapping, scene loading, the render engine and DSP. Depends on psv only.",
     "You are changing how state becomes sound."],
    ["core/", "The composition root and the C ABI implementation. The only place both engines are linked.",
     "You are changing lifecycle, threading or the ABI."],
    ["harness/", "Desktop harness, written against the C ABI only. The worked integration reference.",
     "You want a runnable example, or an offline render."],
    ["bindings/dart/", "ffigen-generated bindings plus a hand-written idiomatic wrapper.",
     "You are integrating from Flutter."],
    ["examples/flutter_smoke/", "Minimal Flutter app: start/stop, live meters, audio on a device.",
     "You want an end-to-end mobile example."],
    ["assets/scenes.json", "Scene manifest; stem paths resolve relative to it.",
     "You are changing the musical material."],
    ["tests/", "Unit tests, golden traces, and the firewall and real-time audits.",
     "Always, before and after any change."],
    ["docs/", "The PSV contract, the PGAE consumption spec, the PGHE proposal, perf records, the build plan.",
     "You want the normative version of anything in this guide."],
    ["CLAUDE.md", "The repository constitution: invariants, stack lock, stop-and-ask list.",
     "Read it first, once, properly."],
], [42 * mm, 62 * mm, CW - 104 * mm], font_size=7.8, mono_cols=(0,))

P("If this guide and the documents in <font face='DJM'>docs/</font> ever disagree, the documents win: they are "
  "the contracts under review, and this guide is an explanation of them.", "small")

story.append(PageBreak())

# ============================================================ APPENDICES
H1("Appendix A &mdash; ABI function reference")

table(["Function", "Thread", "Purpose"], [
    ["prism_version()", "any", "\"MAJOR.MINOR.PATCH\" of the linked library; compare against the header macros."],
    ["prism_result_description(r)", "any", "Static human-readable string for a result code."],
    ["prism_config_default()", "any", "Config with all defaults (Aqademiq profile, UTC, spec cadence)."],
    ["prism_create(cfg, &amp;out)", "control", "Create an engine. cfg may be NULL."],
    ["prism_destroy(core)", "control", "Stop everything owned by the library and free. NULL is a no-op."],
    ["prism_load_scene(core, path)", "control", "Decode and preload every stem. Blocking I/O happens here."],
    ["prism_start(core)", "control", "Emit the neutral vector; launch the inference thread."],
    ["prism_stop(core)", "control", "Stop inference and the built-in device. Idempotent."],
    ["prism_report_app_switch(core, t)", "any", "One context switch at epoch-ms t."],
    ["prism_report_idle(core, t, idle)", "any", "Idle heartbeat: at t, idle for idle ms."],
    ["prism_report_task_deadlines(core, ts, n)", "any", "Replace the task snapshot. n &lt;= 4096."],
    ["prism_get_psv(core, &amp;out)", "any", "Copy the most recent emitted vector."],
    ["prism_set_mood_override(core, &amp;o)", "any", "Pin the vector; published immediately."],
    ["prism_clear_mood_override(core)", "any", "Release the pin; inference resumes next tick."],
    ["prism_crossfade_scene(core, &amp;swap)", "control", "Decode a new scene and arm an equal-power swap."],
    ["prism_crossfade_active(core)", "any", "Non-zero while a swap is armed or running."],
    ["prism_sample_rate(core)", "any", "Sample rate of the loaded scene; 0 before load."],
    ["prism_render(core, out, frames)", "audio", "Render mono float32. Wait-free, allocation-free."],
    ["prism_device_start(core)", "control", "Open and start the built-in playback device."],
    ["prism_device_stop(core)", "control", "Stop and close it. Idempotent."],
], [58 * mm, 16 * mm, CW - 74 * mm], font_size=7.8, mono_cols=(0,))

H1("Appendix B &mdash; Constant reference")

table(["Constant", "Value", "Where"], [
    ["Deadline decay tau", "48 h", "pce/include/pce/deadline.h"],
    ["Priority weights (low/med/high)", "0.4 / 0.7 / 1.0", "pce/include/pce/deadline.h"],
    ["Deadline confidence", "0.9 (0 when list empty)", "pce/include/pce/deadline.h"],
    ["Behavioural window", "120 s", "pce/include/pce/attention.h"],
    ["Expected heartbeat spacing", "5 s (24 samples per window)", "pce/include/pce/attention.h"],
    ["Idle threshold", "15 s", "pce/include/pce/attention.h"],
    ["Switch scale", "6 per minute", "pce/include/pce/attention.h"],
    ["Behavioural confidence ceiling", "0.7 x coverage", "pce/include/pce/attention.h"],
    ["Circadian peak / trough / dip", "16:00 / 04:00 / 14:00", "pce/src/circadian.cpp"],
    ["Circadian confidence", "0.5 (fixed)", "pce/src/circadian.cpp"],
    ["Fusion importances (arousal)", "deadline 1.0, circadian 0.7, behaviour 0.7", "pce/src/fusion.cpp"],
    ["Fusion importances (load)", "deadline 1.0, scatter 0.9", "pce/src/fusion.cpp"],
    ["Fusion importances (readiness)", "circadian 1.0, inverse deadline 0.4", "pce/src/fusion.cpp"],
    ["Emit cadence / significant delta", "30 s / 0.1", "PceOptions, overridable via prism_config"],
    ["Inference tick", "5 s", "prism_config.check_interval_ms"],
    ["Brightness", "0.55 + 0.9a - 0.8l", "pgae/src/mapping.cpp"],
    ["Cutoff range", "300 Hz .. 12 kHz (log)", "pgae/src/mapping.cpp"],
    ["Density", "0.5 + 0.9a - 1.0l", "pgae/src/mapping.cpp"],
    ["Density gates (pulse/air/lead)", "0.35 / 0.55 / 0.72", "pgae/src/mapping.cpp"],
    ["Gain to amplitude", "0.4 x g^1.5", "pgae/src/engine.cpp"],
    ["Smoothing (gain / cutoff)", "0.25 s / 0.6 s", "pgae/include/pgae/engine.h"],
    ["Density crossfade", "1.5 s, equal power", "pgae/include/pgae/engine.h"],
    ["Master gain / fade-in", "0.7 / 0.5 s", "pgae/include/pgae/engine.h"],
    ["Limiter", "-3 dBFS, 3 ms attack, 100 ms release", "pgae/include/pgae/engine.h"],
], [58 * mm, 52 * mm, CW - 110 * mm], font_size=7.8, mono_cols=(2,))

H1("Appendix C &mdash; Glossary")

table(["Term", "Meaning"], [
    ["PCE", "Prism Context Engine. Turns raw events into a state vector. Knows nothing about audio."],
    ["PSV", "Prism State Vector. Four dimensions, each a value plus its own confidence. The only thing that "
     "crosses between the two engines."],
    ["PGAE", "Prism Generative Audio Engine. Turns a state vector into sound. Knows nothing about inputs."],
    ["PGHE", "Prism Generative Haptics Engine &mdash; specified, <b>not implemented</b>. A second consumer of "
     "the same vector."],
    ["Effective value", "A dimension collapsed with its confidence: 0.5 + (value - 0.5) x confidence. What "
     "actually drives output."],
    ["The firewall", "The structural rule that the PCE emits only a state vector and the PGAE reads only a "
     "state vector, enforced by the build graph and a test."],
    ["Scatter", "How fragmented attention is, from window-switch frequency. 0 settled, 1 constant switching."],
    ["Activity", "The fraction of recent idle samples that were below the away threshold. 1 present, 0 away."],
    ["Density", "A mapped scalar that gates how many stem layers are audible."],
    ["Stem", "One pre-decoded audio loop with a role: bed, sub, pulse, lead or air."],
    ["Scene", "A named set of stems in one musical key, described by scenes.json."],
    ["Pull model", "You own the audio device and call prism_render. The alternative is the built-in device."],
    ["Golden trace", "A recorded input trace plus the state trajectory the validated probe produced from it; "
     "the parity bar is 1e-6."],
], [30 * mm, CW - 30 * mm], font_size=8.0)

SP(10)
P("This document describes commit <font face='DJM'>2f55a27</font>. When the engine changes, the tables in "
  "Part II can be regenerated by rebuilding and re-running the probe that produced them; the invariants in "
  "Part III change only by deliberate decision.", "small")


# ============================================================ BUILD
def decorate(canvas, doc):
    canvas.saveState()
    if doc.page > 1:
        canvas.setFont("DJ", 7.6)
        canvas.setFillColor(MUTED)
        canvas.drawString(MARGIN, 12 * mm, "Prism Core — Engine Guide")
        canvas.drawRightString(PW - MARGIN, 12 * mm, "%d" % doc.page)
        canvas.setStrokeColor(RULE)
        canvas.setLineWidth(0.4)
        canvas.line(MARGIN, 15 * mm, PW - MARGIN, 15 * mm)
    else:
        canvas.setFillColor(ACCENT)
        canvas.rect(0, PH - 14 * mm, PW, 14 * mm, stroke=0, fill=1)
    canvas.restoreState()


OUT = "/home/user/prism-core/Prism-Core-Engine-Guide.pdf"
doc = BaseDocTemplate(OUT, pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN,
                      topMargin=MARGIN, bottomMargin=22 * mm,
                      title="Prism Core - Engine Guide",
                      author="Prism", subject="Engine data flow, modification and integration guide")
frame = Frame(MARGIN, 22 * mm, CW, PH - MARGIN - 22 * mm, id="body",
              leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
doc.addPageTemplates([PageTemplate(id="main", frames=[frame], onPage=decorate)])
doc.build(story)
print("wrote", OUT)
