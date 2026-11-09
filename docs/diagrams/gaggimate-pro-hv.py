"""Reverse-engineered schematic of the GaggiMate Pro Rev 1.1 mains section.

Traced from photos of the board underside; values come from SMD markings.
Unmarked MLCC values and the LED-side routing of U4/U5 are not confirmed.

Render with:  python gaggimate-pro-hv.py   (needs schemdraw + matplotlib)
"""

import schemdraw
import schemdraw.elements as elm

USER_MOD = "#1f5fbf"
OFFBOARD = "#777777"

schemdraw.config(fontsize=11, unit=2.5)

d = schemdraw.Drawing(show=False)

TOP, BOT = 0.0, -18.0  # fused line rail, neutral rail


def dot(xy, label=None, loc="left", color=None):
    e = elm.Dot().at(xy)
    if color:
        e.color(color)
    if label:
        e.label(label, loc=loc)
    d.add(e)


def wire(*pts, color=None):
    for a, b in zip(pts, pts[1:]):
        w = elm.Line().endpoints(a, b)
        if color:
            w.color(color)
        d.add(w)


# ---------------------------------------------------------------- mains in
dot((0, TOP), "J2 Line", loc="left")
d.add(elm.Fuse().endpoints((0, TOP), (4, TOP)).label("F1\n1 A", loc="top"))
wire((4, TOP), (42, TOP))
d.add(elm.Label().at((22, TOP + 0.6)).label("L (fused)", fontsize=10))

dot((0, BOT), "J2 Neutral", loc="left")
wire((0, BOT), (42, BOT))
d.add(elm.Label().at((22, BOT - 0.7)).label("N", fontsize=10))

# PS1, on-board AC/DC module (top side), fed after F1
XP = 5.5
ps = d.add(elm.Ic(pins=[elm.IcPin(name="AC1", side="left", slot="2/2"),
                        elm.IcPin(name="AC2", side="left", slot="1/2"),
                        elm.IcPin(name="+5V", side="right", slot="2/2"),
                        elm.IcPin(name="GND", side="right", slot="1/2")],
                   size=(2.6, 3)).at((XP + 1.0, -7.5)).anchor("AC1")
                   .label("PS1\nAC/DC 5 V", loc="center", fontsize=10))
dot((XP, TOP))
wire((XP, TOP), (XP, ps.AC1[1]), ps.AC1)
wire(ps.AC2, (XP, ps.AC2[1]), (XP, BOT))
dot((XP, BOT))
d.add(elm.Label().at((XP, ps.AC2[1] - 2.2))
      .label("+5V to U2 (3.3 V LDO),\nESP32-S3, pressure, display", halign="left", fontsize=9))

# ---------------------------------------------------------------- pump: D6 + U4
XS, XT = 10.5, 16.0         # snubber column, triac column
YOUT = -8.0                 # TRIAC output node
dot((XS, TOP))
d.add(elm.Resistor().endpoints((XS, TOP), (XS, -4)).label("R4\n220 Ω\n2512", loc="bottom"))
d.add(elm.Capacitor().endpoints((XS, -4), (XS, YOUT)).label("C12\nMLCC\n(value ?)", loc="bottom"))
wire((XS, YOUT), (XT, YOUT))

dot((XT, TOP))
tri = d.add(elm.Triac().endpoints((XT, TOP - 1.5), (XT, YOUT + 1.5)).reverse()
            .label("D6\nfactory: Z0107MN ('Z7M')\nnow: Z0109MN (user)", loc="top", ofst=0.5, fontsize=9))
wire((XT, TOP), (XT, TOP - 1.5))
wire((XT, YOUT + 1.5), (XT, YOUT))

XR5 = XT + 2.4
u4 = d.add(elm.Ic(pins=[elm.IcPin(name="MT", pin="6", anchorname="P6", side="left", slot="2/2"),
                        elm.IcPin(name="MT", pin="4", anchorname="P4", side="left", slot="1/2"),
                        elm.IcPin(name="A", pin="1", side="right", slot="2/2"),
                        elm.IcPin(name="K", pin="2", side="right", slot="1/2")],
                   size=(2.4, 3)).theta(0).at((XR5 + 1.3, -2.5)).anchor("P6")
                   .label("U4\nMOC3021\n400 V,\nrandom-\nphase", loc="center", fontsize=9))
d.add(elm.Resistor().endpoints((XR5, TOP), (XR5, u4.P6[1])).label("R5\n470 Ω", loc="bottom"))
dot((XR5, TOP))
wire((XR5, u4.P6[1]), u4.P6)
wire(u4.P4, (tri.gate[0], u4.P4[1]), tri.gate)
wire(u4.A, (u4.A[0] + 0.8, u4.A[1]))
wire(u4.K, (u4.K[0] + 0.8, u4.K[1]))
d.add(elm.Ground().at((u4.K[0] + 0.8, u4.K[1])))
d.add(elm.Label().at((u4.A[0] + 1.0, u4.A[1] + 0.2))
      .label("GPIO9 (pump)\nvia R28,\nroute not traced", halign="left", fontsize=9))

dot((XT, YOUT))
wire((XT, YOUT), (XT, -10))
dot((XT, -10), "J2 Pump", loc="left")
d.add(elm.Inductor2(loops=3).endpoints((XT, -10), (XT, -13.5)).color(OFFBOARD)
      .label("pump coil\nOlab Silent Green\nK09541L, 35 W", loc="bottom", color=OFFBOARD))
d.add(elm.Diode().endpoints((XT, -13.5), (XT, -16)).color(OFFBOARD)
      .label("internal diode\n(not on datasheet;\nverify with meter)", loc="bottom", color=OFFBOARD))
wire((XT, -16), (XT, BOT), color=OFFBOARD)
dot((XT, BOT))

# ---------------------------------------------------------------- valve: U5 SSR
XS2, XU = 28.0, 31.0
dot((XS2, TOP))
d.add(elm.Resistor().endpoints((XS2, TOP), (XS2, -4)).label("R9\n220 Ω\n2512", loc="bottom"))
d.add(elm.Capacitor().endpoints((XS2, -4), (XS2, YOUT)).label("C13\nMLCC\n(value ?)", loc="bottom"))
wire((XS2, YOUT), (XU, YOUT))

u5 = d.add(elm.Ic(pins=[elm.IcPin(name="T1", side="top"),
                        elm.IcPin(name="T2", side="bot"),
                        elm.IcPin(name="A", pin="1", side="right", slot="2/2"),
                        elm.IcPin(name="K", pin="2", side="right", slot="1/2")],
                   size=(2.6, 3.4)).theta(0).at((XU, -1.6)).anchor("T1")
                   .label("U5\nBC2213A\nopto-TRIAC\nSSR", loc="center", fontsize=9))
dot((XU, TOP))
wire((XU, TOP), u5.T1)
wire(u5.T2, (XU, YOUT))
wire(u5.A, (u5.A[0] + 0.8, u5.A[1]))
wire(u5.K, (u5.K[0] + 0.8, u5.K[1]))
d.add(elm.Ground().at((u5.K[0] + 0.8, u5.K[1])))
d.add(elm.Label().at((u5.A[0] + 1.0, u5.A[1] + 0.2))
      .label("GPIO10 (valve)\nvia Q1 2N7002K\n+ R7, inferred", halign="left", fontsize=9))

dot((XU, YOUT))
wire((XU, YOUT), (XU, -10))
dot((XU, -10), "J2 Valve", loc="left")
# FB1: clamp-on ferrite around both solenoid leads near the valve (common-mode as fitted)
wire((XU, -10), (XU, -10.6), color=OFFBOARD)
d.add(elm.Inductor2(loops=2).endpoints((XU, -10.6), (XU, -12.0)).color(USER_MOD))
d.add(elm.Inductor2(loops=3).endpoints((XU, -12.0), (XU, -15.0)).color(OFFBOARD)
      .label("solenoid coil\nOlab 08252L50-A14-A-G\n(8000BH coil, ~15 VA)", loc="bottom", color=OFFBOARD))
d.add(elm.Inductor2(loops=2).endpoints((XU, -15.0), (XU, -16.4)).color(USER_MOD))
d.add(elm.Line().endpoints((XU - 0.75, -10.8), (XU - 0.75, -16.2)).color(USER_MOD).linestyle("--"))
d.add(elm.Label().at((XU - 0.9, -13.5)).label("FB1\nclamp-on\nferrite,\nboth leads\n(user-added)",
      halign="right", fontsize=9, color=USER_MOD))
wire((XU, -16.4), (XU, BOT), color=OFFBOARD)
dot((XU, BOT))

XM = XU + 6.0
wire((XU, -10), (XM, -10), color=USER_MOD)
d.add(elm.Resistor().endpoints((XM, -10), (XM, BOT)).color(USER_MOD)
      .label("RV1 MOV\nS10K275\n(user-added)", loc="bottom", color=USER_MOD))
d.add(elm.Line().at((XM - 0.7, -15.2)).to((XM + 0.7, -12.8)).color(USER_MOD))
dot((XM, BOT), color=USER_MOD)

# ---------------------------------------------------------------- zero-cross detector
XZ = 42.0
YA = -6.0
dot((XZ, TOP))
d.add(elm.Resistor().endpoints((XZ, TOP), (XZ, YA + 1.5)).label("R26\n220 kΩ", loc="top"))
dot((XZ, BOT))
br = d.add(elm.Ic(pins=[elm.IcPin(name="AC1", pin="~", side="left", slot="2/2"),
                        elm.IcPin(name="AC2", pin="~", side="left", slot="1/2"),
                        elm.IcPin(name="P", pin="+", side="right", slot="2/2"),
                        elm.IcPin(name="M", pin="−", side="right", slot="1/2")],
                   size=(2.0, 3)).theta(0).at((XZ + 0.8, YA)).anchor("AC1")
                   .label("D2", loc="center", fontsize=10))
wire((XZ, YA + 1.5), (XZ, br.AC1[1]), br.AC1)
d.add(elm.Resistor().endpoints((XZ, BOT), (XZ, br.AC2[1] - 1.5)).label("R27\n220 kΩ", loc="top"))
wire((XZ, br.AC2[1] - 1.5), (XZ, br.AC2[1]), br.AC2)

yp, ym = br.P[1], br.M[1]
xp = br.P[0] + 1.0
wire(br.P, (xp, yp))
dot((xp, yp))
xe = xp + 3.0
d.add(elm.Diode().endpoints((xp, yp), (xe, yp)).label("D3", loc="bottom"))
dot((xe, yp))
d.add(elm.Capacitor().endpoints((xe, yp), (xe, ym - 2.0)).label("C21", loc="bottom"))
wire(br.M, (br.M[0] + 0.5, ym), (br.M[0] + 0.5, ym - 2.0), (xe, ym - 2.0))
dot((xe, ym - 2.0))

q3 = d.add(elm.BjtPnp(circle=True).flip().at((xe + 1.2, yp + 3.0)).anchor("base")
           .label("Q3", loc="right"))
d.add(elm.Resistor().endpoints((xp, yp), (xp, q3.base[1])).label("R29\n~20 kΩ", loc="top"))
wire((xp, q3.base[1]), q3.base)
wire(q3.emitter, (q3.emitter[0], yp), (xe, yp))
yc = q3.collector[1] + 0.8
wire(q3.collector, (q3.collector[0], yc))
d.add(elm.Resistor().endpoints((q3.collector[0], yc), (q3.collector[0] + 3.0, yc)).label("R30 1 kΩ", loc="top"))
u10 = d.add(elm.Optocoupler().at((q3.collector[0] + 3.0, yc)).anchor("anode")
            .label("U10", loc="top"))
wire(u10.cathode, (u10.cathode[0], ym - 2.0), (xe, ym - 2.0))
wire(u10.collector, (u10.collector[0] + 0.8, u10.collector[1]))
d.add(elm.Label().at((u10.collector[0] + 1.0, u10.collector[1] + 0.3))
      .label("GPIO21 pump sense\n(zero-cross pulse)\nvia R33", halign="left", fontsize=9))
wire(u10.emitter, (u10.emitter[0] + 0.8, u10.emitter[1]))
d.add(elm.Ground().at((u10.emitter[0] + 0.8, u10.emitter[1])))

# ---------------------------------------------------------------- notes
d.add(elm.Label().at((0, BOT - 3.2)).label(
    "GaggiMate Pro Rev 1.1, mains section, reverse-engineered from board photos (2026-11-07).  "
    "Drawn from a Rev 1.1 board; a second board with the factory D6 shows the same layout.  "
    "Grey: off-board machine loads.  Blue: user modification (MOV across Valve and shared Neutral, confirmed).\n"
    "Not confirmed: C12/C13/C21 values (unmarked MLCC), LED-side routing of U4/U5 and R28 value (MOC3021 IFT up to 15 mA), "
    "F1 in Line (by terminal mapping).  No gate-to-MT1 resistor on D6 and no MOV/TVS on the stock board.\n"
    "FB1 as fitted passes valve and neutral leads together (common-mode only); planned re-wind takes the valve lead alone "
    "through 2 to 4 turns as a series choke.  U4 (400 V) sees the full blocking voltage across D6.",
    halign="left", fontsize=9))

if __name__ == "__main__":
    d.save("gaggimate-pro-hv.svg")
    d.save("gaggimate-pro-hv.png", dpi=110)
