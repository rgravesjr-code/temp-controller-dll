"""Generate the monochrome, vector-based TempSim operator manual."""
from pathlib import Path
from xml.sax.saxutils import escape
from reportlab.pdfgen import canvas
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak, Flowable
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_LEFT
from reportlab.lib import colors
from reportlab.lib.pagesizes import letter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'output/pdf/TempSim_2.2.1_User_Manual_Print.pdf'
OUT.parent.mkdir(parents=True, exist_ok=True)
W = 504
styles = {
 'title': ParagraphStyle('title', fontName='Helvetica-Bold', fontSize=25, leading=29, spaceAfter=14),
 'h1': ParagraphStyle('h1', fontName='Helvetica-Bold', fontSize=19, leading=23, spaceAfter=13),
 'h2': ParagraphStyle('h2', fontName='Helvetica-Bold', fontSize=12, leading=15, spaceBefore=10, spaceAfter=6),
 'body': ParagraphStyle('body', fontName='Helvetica', fontSize=10.5, leading=14.2, spaceAfter=8),
 'small': ParagraphStyle('small', fontName='Helvetica', fontSize=9.4, leading=12, spaceAfter=6),
 'cell': ParagraphStyle('cell', fontName='Helvetica', fontSize=9.5, leading=12),
 'head': ParagraphStyle('head', fontName='Helvetica-Bold', fontSize=9.5, leading=12),
 'code': ParagraphStyle('code', fontName='Courier', fontSize=9, leading=12, spaceAfter=8),
}
story=[]
def p(text, style='body'): story.append(Paragraph(text, styles[style]))
def h(text): p(text,'h2')
def page(text):
    if story: story.append(PageBreak())
    p(text,'h1')
def steps(items):
    for i,item in enumerate(items,1): p(f'<b>{i}.</b> {item}')
def table(headers, rows, widths):
    data=[[Paragraph(escape(str(v)),styles['head']) for v in headers]]
    data += [[Paragraph(escape(str(v)),styles['cell']) for v in row] for row in rows]
    t=Table(data,colWidths=widths,hAlign='LEFT',repeatRows=1)
    t.setStyle(TableStyle([
      ('VALIGN',(0,0),(-1,-1),'TOP'),('LINEABOVE',(0,0),(-1,0),1,colors.black),
      ('LINEBELOW',(0,0),(-1,0),.8,colors.black),('LINEBELOW',(0,1),(-1,-1),.35,colors.black),
      ('TOPPADDING',(0,0),(-1,-1),5),('BOTTOMPADDING',(0,0),(-1,-1),5),
      ('LEFTPADDING',(0,0),(-1,-1),5),('RIGHTPADDING',(0,0),(-1,-1),5)]))
    story.append(t); story.append(Spacer(1,9))

class Diagram(Flowable):
    def __init__(self,kind,height): super().__init__(); self.kind=kind; self.width=W; self.height=height
    def draw(self):
        c=self.canv; c.setStrokeGray(0); c.setFillGray(0); c.setLineWidth(.9)
        def text(x,y,s,size=10,bold=False):
            c.setFont('Helvetica-Bold' if bold else 'Helvetica',size); c.drawCentredString(x,y,s)
        def box(x,y,w,ht,lines):
            c.rect(x,y,w,ht)
            for i,s in enumerate(lines): text(x+w/2,y+ht-16-i*14,s,10,i==0)
        def arrow(x1,y1,x2,y2,dashed=False):
            import math
            c.setDash(4,3) if dashed else c.setDash()
            c.line(x1,y1,x2,y2); c.setDash()
            a=math.atan2(y2-y1,x2-x1)
            for da in (-.45,.45): c.line(x2,y2,x2-6*math.cos(a+da),y2-6*math.sin(a+da))
        if self.kind=='oil':
            box(14,110,108,49,['INLINE HEATER','Adds supply heat'])
            box(188,94,136,76,['UNIT UNDER TEST','Actuation adds heat','Outlet thermal mass'])
            box(401,108,89,50,['EXTERNAL','FAN'])
            box(188,210,136,45,['MOTORS M1-M3','Drive the UUT'])
            arrow(256,210,256,170); text(340,188,'Mechanical drive',9)
            arrow(401,132,324,132,True); text(362,147,'Airflow',9)
            arrow(122,133,188,133); text(153,152,'INLET',9,True)
            arrow(324,106,357,106); c.line(357,106,357,38)
            arrow(357,38,122,38); text(242,48,'OUTLET / RETURN OIL',9,True)
            box(14,14,108,49,['OIL PUMP','Recirculates oil'])
            arrow(68,63,68,110)
            c.circle(339,106,3,stroke=1,fill=0)
            text(428,79,'T1 and T2 at outlet',9)
            text(252,279,'Oil path and equipment roles (conceptual)',11,True)
        elif self.kind=='control':
            box(0,80,117,58,['OUTLET PROBES','T1 and T2','Offset / lag / noise'])
            box(171,80,145,58,['REAL TEMPCTL DLL','Validate and filter','Decide heat / cool'])
            box(368,80,136,58,['RELAY MODEL','Command + feedback','Drive heater / fan'])
            arrow(117,108,171,108); arrow(316,108,368,108)
            box(171,0,145,44,['THERMAL MODEL','New inlet and outlet'])
            c.line(436,80,436,22); arrow(436,22,316,22)
            c.line(171,22,58,22); arrow(58,22,58,80)
            text(252,158,'Repeated every simulation tick (default: 100 ms)',10,True)
        elif self.kind=='band':
            x0,x1=70,455
            for y,label,dash in [(127,'55 C  Upper deadband',[3,3]),(83,'50 C  Setpoint',[]),(39,'45 C  Lower deadband',[3,3])]:
                c.setDash(dash); c.line(x0,y,x1,y); c.setDash(); text(252,y+7,label,10,True)
            text(252,154,'Above 55 C: request cooling after the start delay',10)
            text(252,15,'Below 45 C: request heating after the start delay',10)
            c.line(50,33,50,137); arrow(50,125,50,143); text(23,81,'C',10)
        elif self.kind=='screen':
            box(0,198,504,41,['A  TOOLBAR','Pause / speed / fixture configuration / motor command / scenario'])
            box(0,143,504,44,['B  LIVE READOUTS','Setpoint, probes, inlet, outlet, signed difference, heat and time'])
            box(0,0,322,132,['C  GRAPH + FIXTURE','Temperature history (upper area)','Animated oil circuit (lower area)','Motors have finned housings and shafts'])
            box(334,0,170,132,['D  RIGHT-HAND TABS','Live status','Configuration','CAN data','Scroll to reach more controls'])
        elif self.kind=='meter':
            def meter(y,label,fraction,caption):
                c.setFont('Helvetica-Bold',10); c.drawString(0,y+26,label)
                c.setFont('Helvetica',10); c.drawString(0,y+10,caption)
                c.rect(0,y-4,504,5); c.rect(0,y-4,504*fraction,5,stroke=0,fill=1)
            meter(128,'Idle timer',0,'Idle - not timing (delay 500 ms)')
            meter(75,'Active countdown',.62,'Timing - 3100 / 5000 ms remaining; bar drains')
            meter(22,'Invalid-reading accumulator',.4,'800 / 2000 ms accumulated; fills with invalid readings')

# 1
p('TempSim 2.2.1','title')
p('User manual and operating instructions','h1')
p('Windows thermal test-bench simulator | Print edition | 20 September 2026','small')
h('What this program does')
p('TempSim shows how a temperature-controlled oil circuit responds to heater power, UUT actuation, oil flow and fan cooling. It combines the <b>real TempCtl controller DLL</b> with an estimated physical model. You can watch temperatures, change settings, stop or start motors, and try controller faults without connected hardware.')
p('<b>UUT</b> means unit under test. M1, M2 and M3 are drive motors, not fans. The separately labeled external fan cools the fixture. The heater and fan can switch while the motors continue running.')
h('Use this guide')
table(['Page','Topic'],[(2,'First run and a five-minute demonstration'),(3,'Oil circuit and inlet/outlet temperatures'),(4,'How the temperature controller works'),(5,'Screen layout and main controls'),(6,'Timer bars, accumulators and lamps'),(7,'Step-by-step operating exercises'),(8,'Motor and oil-loop settings'),(9,'Fan, ambient, sensors and relay settings'),(10,'Controller settings reference'),(11,'Logging, saved settings and test scenarios'),(12,'Troubleshooting, glossary and model limits')],[45,459])
h('Printing and scope')
p('Print on US Letter paper, portrait, at 100% size. Duplex printing on the long edge is suitable. All diagrams use black lines, white backgrounds and written labels; no meaning depends on color. A4 printers may use Fit to printable area. This guide covers the 2.2.1 Windows interface.', 'small')

# 2
page('2. First run')
steps(['Save the supplied ZIP locally. Use <b>Extract All</b> to extract the entire archive. Do not run from inside the ZIP.',
'Open the extracted folder and double-click <b>Run TempSim.cmd</b>. Alternatively, open the TempSim subfolder and run TempSim.exe. Keep all files in that subfolder together.',
'Check the version line at the bottom: <b>TempSim 2.2.1</b>. If an older version is open, close it and launch the newly extracted copy.',
'Select <b>Run fixture</b> to start a fresh fixture simulation. If the top-left button says Run, click it; if it says Pause, the simulation is already running.',
'Choose <b>10x</b> speed. Watch the heater turn on, the inlet warm first, and the outlet follow. The two controller probes measure the outlet.',
'Click <b>Stop motors</b>. Their speed coasts down, while oil circulation and temperature control continue. Click <b>Start motors</b> to resume actuation.',
'Click <b>Pause</b> to inspect values. Click Run to continue. Close the application normally when finished; its main configuration is saved.'])
h('Starting values you should recognize')
table(['Item','Factory estimate'],[('Setpoint / lower band / upper band','50 C / 45 C / 55 C'),('Signal validity limits','10 C minimum; 90 C maximum'),('Initial temperature and ambient','20 C'),('Drive motors','2 enabled; 1800 rpm command'),('Oil pump','Enabled; 12 L/min command'),('Heater / fan','6 kW heater; fan automatic from cooler relay')],[240,264])
p('Saved settings can change these values. To return to factory settings, follow the recovery procedure on page 12. Higher simulation speed changes how quickly simulated time passes, not the controller setpoint or the physical model.', 'small')
h('Computer requirements')
p('Use 64-bit Windows. The package includes its .NET runtime and required native libraries. No .NET SDK, LabVIEW installation, CAN adapter or connected test fixture is needed for the graphical simulation.')

# 3
page('3. The oil circuit')
story.append(Diagram('oil',295))
p('<b>Inlet</b> is the modeled supply temperature entering the UUT. <b>Outlet</b> is the modeled UUT oil/fixture temperature leaving it. Oil recirculates, so heat from the UUT can return to the supply.')
p('The model has two internally mixed thermal masses: supply oil, and UUT oil plus fixture metal. The total oil volume includes the oil held inside the UUT. Fan and passive heat exchange act at the UUT. Heater power enters the supply; motor actuation heat enters the UUT.')
h('What the temperature difference means')
table(['Readout','Interpretation'],[('Delta T = outlet - inlet','Positive: outlet is hotter. Negative: inlet is hotter.'),('During heater warm-up','The inlet may be hotter than the outlet. A negative difference is expected.'),('During UUT actuation','Motor work can heat the outlet. Flow and heat losses determine how large the rise becomes.'),('When pump flow increases','Heat moves between the two masses more quickly; the actuation difference generally gets smaller.')],[168,336])
p('<b>Both T1 and T2 are outlet probes.</b> They are redundant controller inputs, each with its own sensor errors. Inlet/outlet are additional physical-model channels. Do not interpret T1 as inlet and T2 as outlet.', 'small')

# 4
page('4. How temperature control works')
story.append(Diagram('control',179))
p('TempCtl validates and filters sensor readings, selects a usable probe, then commands heating or cooling. The simulated relays provide feedback. Each tick also packs and unpacks the diagnostics through CanTp as a communication check.')
story.append(Diagram('band',174))
p('With the default setup, a temperature <b>below 45 C</b> starts a heating request; a temperature <b>above 55 C</b> starts a cooling request. The request must survive the 500 ms relay-start delay. When neither relay is on and the temperature is within the band, no new relay is requested.')
p('Once heating is on, it continues toward the <b>50 C setpoint</b>. At or above setpoint, the 500 ms at-setpoint release delay runs before heating switches off. Cooling similarly releases after temperature remains at or below setpoint for its release delay. Thermal inertia can cause overshoot.')
h('Independent equipment commands')
p('The motors and oil pump have their own run commands. They do not stop just because the heater switches off. In automatic mode, the external fan follows the cooler relay; manual fan mode uses its own command. A controller fault is not a complete machine shutdown interlock.')
p('Countdowns start when a qualifying condition is first observed; that first tick does not consume the delay. At the default 100 ms tick, short timers may appear only briefly, especially at 10x or 20x speed.', 'small')

# 5
page('5. Screen and control reference')
story.append(Diagram('screen',252))
table(['Control / area','How to use it'],[
('Pause / Run','Freeze or resume the whole simulation, including animation. This does not reset it.'),
('Speed','1x, 2x, 3x, 5x, 10x or 20x. The displayed simulated time is authoritative if the computer cannot keep up.'),
('Stop motors / Start motors','Change the shared run command for all selected motors. Stopped motors coast down; they do not freeze instantly.'),
('Fixture configuration','Open physical settings. The simulation pauses while the dialog is open. Apply physics commits valid edits together; closing cancels them.'),
('Reset fault','Clear ordinary controller faults/history and drop relay commands. Keep setup and thermal state. ConfigFault requires valid reinitialization.'),
('Restart','Start a new run from configured initial temperature using current settings. Reset plant/controller history.'),
('Run fixture','Exit a scripted scenario and start a fresh fixture run. If paused, click Run afterward.'),
('Test scenario + Load','Select a scripted controller test and load it. These tests use the legacy plant, not the inlet/outlet fixture model.')],[140,364])
p('The graph retains the last 120 simulated seconds. Signal max/min are configured validity limits, not observed extrema. The plant curve is the heat-capacity-weighted mean of inlet and outlet. The lower lanes show heater and cooler commands.', 'small')

# 6
page('6. Read the status indicators')
story.append(Diagram('meter',171))
table(['Indicator','Meaning'],[
('Relay start delay','Remaining time before an out-of-band request turns heating or cooling on.'),
('At-setpoint release delay','Remaining time before the active relay releases after its setpoint condition is met.'),
('Sensor disagreement fault delay','Remaining time before sustained qualified disagreement becomes a fault. Comparison is gated by controller state; it is not always active.'),
('Heater / cooler feedback fault delay','Remaining time before command-versus-feedback disagreement becomes a fault.'),
('Sensor 1 / 2 invalid-reading accumulation','Accumulated invalid-reading time. Fills on every invalid tick and drains at half the elapsed time during valid readings.')],[191,313])
h('Read the words as well as the bars')
p('<b>Idle</b> means no countdown is running. <b>Clear</b> means zero invalid-reading accumulation. <b>Disabled</b> means the check is not enabled. <b>Timing</b> shows remaining milliseconds. <b>Fault snapshot</b> means the displayed diagnostic values are frozen at the fault event.')
p('An empty track during normal heating is usually correct: the relay-start delay has finished, and the at-setpoint delay has not started yet. Try 1x speed to observe short countdowns. The on-screen colors reinforce these written states; they are not required to understand them.')
h('Relay lamps and sensor selection')
p('<b>doHeater / doCooler</b> are commands. <b>Heater DO fb / Cooler DO fb</b> are simulated physical feedback contacts. Active sensor identifies the selected controller probe. A failed probe is not automatically readmitted when its signal recovers; remove the injected problem and reset or reinitialize.')

# 7
page('7. Step-by-step operating exercises')
h('A. Change the target temperature')
steps(['Use <b>Run fixture</b>. Open the <b>Configuration</b> tab and find Setpoint.',
'Enter a target within the signal limits and press Enter. Keep deadband offsets valid. The change reinitializes the controller with the entire setup; it can retain a running relay.',
'Watch the new band and thermal response. The default band is setpoint minus 5 to setpoint plus 5. Select 1x if you want to see individual countdowns.'])
h('B. See heat from UUT actuation')
steps(['Open <b>Fixture configuration</b>. Keep pump running. Note motor count, power, load fraction and heat fraction; click Apply physics.',
'Compare inlet, outlet, signed difference and Actuation heat. Use Stop motors, then Start motors, and allow time for the temperatures to respond.',
'To make the effect easier to isolate, compare runs with the same heater/fan conditions. Heater warm-up can mask the actuation rise. Increase load or heat fraction deliberately, one change at a time.'])
h('C. Change oil flow or run the external fan')
steps(['For flow: change Pump target flow in Fixture configuration, apply, and observe the inlet/outlet difference after it settles.',
'For a manual fan: clear Automatic fan, enable Manual fan running, set a command RPM no higher than rated RPM, and apply.',
'Restore Automatic fan when finished. Air cooling approaches ambient; it is not modeled as refrigeration. Turning the pump off can leave actuation heat trapped in the UUT.'])
h('D. Try a sensor fault and recover')
steps(['At 1x speed, open Configuration and Sensor 1. Set Fault to Open. Watch its accumulation and eventual failover if Sensor 2 remains usable.',
'Set Fault back to None and clear any Override. Click Reset fault. A recovered sensor is not readmitted merely by restoring its reading.',
'For a predefined test, load failover. Use Run fixture afterward to leave its timed events and return to fixture physics.'])

# 8
page('8. Motor and oil-loop settings')
p('Open <b>Fixture configuration</b>, scroll to the required group, edit values, then click <b>Apply physics</b>. These are starting estimates, not measured machine data. Values below are factory defaults; saved settings may differ.')
table(['Setting','Default','What it changes'],[
('Use fixture physics','On in desktop','Off selects the legacy plant model.'),
('Motors running / motor count','On / 2','Shared command; choose 1, 2 or 3 drives.'),
('Command / rated motor speed','1800 / 3000 rpm','Actual speed approaches command; command must not exceed rated speed.'),
('Rated power per motor','1500 W','Basis for actuation heating.'),
('Load / heat fraction','0.6 / 0.25','Fractions from 0 to 1. More load or heat fraction adds more UUT heat.'),
('Motor response time','2 s','How gradually motor RPM changes.'),
('Oil pump running','On','Independent circulation command.'),
('Pump flow / response','12 L/min / 1 s','Flow target and approach time.'),
('Heater rated power','6000 W','Maximum power before the flow-dependent transfer factor.'),
('Flow for 50% heater transfer','4 L/min','At this flow, half the rated heater power reaches supply oil.'),
('Total oil / oil inside UUT','8 L / 0.5 L','UUT hold-up is part of the total, not added to it.'),
('Oil density','0.85 kg/L','Converts oil volume into mass.'),
('Oil specific heat','2000 J/(kg K)','Energy needed to heat the oil.'),
('Fixture mass / specific heat','12 kg / 500 J/(kg K)','Thermal storage at the UUT/outlet.'),
('Passive heat loss','8 W/K','Heat exchange between UUT and ambient without the fan.')],[176,108,220])
h('A useful numerical example')
p('At steady default speed, actuation heat is approximately <b>270 W</b>: 2 motors x 1500 W x 0.6 load x 0.25 heat fraction x (1800/3000). With heater on at 12 L/min, modeled delivery is <b>4500 W</b>: 6000 x 12/(12 + 4). These powers enter different thermal masses.', 'small')
p('Oil volumes, density and heat capacities must be positive; UUT oil volume must be smaller than total volume. Each thermal mass must have at least 1 J/K capacity. The dialog rejects invalid combinations.', 'small')

# 9
page('9. Fan, ambient and sensor settings')
table(['Fixture configuration: fan','Default','Meaning'],[
('Automatic fan','On','Follow physical cooler-relay state.'),('Manual fan running','On','Used only when automatic mode is off.'),('Command / rated fan speed','1600 / 2000 rpm','Command must not exceed rated speed.'),('Cooling at rated speed','120 W/K','Additional heat exchange at full rated speed.'),('Fan response time','1.5 s','How gradually RPM changes.')],[177,108,219])
h('Configuration tab: ambient and legacy plant')
p('<b>Ambient</b> applies to fixture heat exchange; default 20 C. <b>Set entire loop temperature now</b> sets both thermal masses to the entered temperature. Heat rate, cool rate and lag-to-ambient fields belong to the <b>legacy plant</b>; change heater power and fan conductance in Fixture configuration for the new model.')
h('Configuration tab: Sensor 1 and Sensor 2')
table(['Setting','Default / use'],[
('Offset','Sensor 1: +0.3; Sensor 2: -0.2 degrees. This is sensor measurement bias, separate from controller Temp2Offset.'),
('Noise amplitude','0. Adds deterministic simulated noise when increased.'),
('Lag (1/s)','0 = no lag. A positive value makes the reading approach its input over time.'),
('Fault','None, Open (not a number), StuckLast, or StuckValue. Choose None to remove injection.'),
('Stuck value','Value used by StuckValue fault.'),
('Override + slider','Forces the sensor input manually. Clear Override to return to the physical model.')],[128,376])
h('Configuration tab: relay feedback')
p('Each heater/cooler relay has <b>stuck open</b>, <b>stuck closed</b>, and <b>answer delay in ticks</b>. Defaults are no faults and zero delay. At the default period, one tick is 100 ms. Restore normal relay settings before resetting a feedback fault; otherwise it can recur.')
p('Fixture edits preserve current temperatures and speeds. Entering fixture physics from a scripted scenario cancels its events/profile and restarts controller state at the current plant temperature. Main settings are saved on normal exit, but temporary fault injections and overrides are not all persisted.', 'small')

# 10
page('10. Controller settings reference')
p('The <b>Configuration</b> tab exposes all 17 controller setup values. Enter or leaving a numeric field applies it through TcInit. Use a decimal point. Changing temperature units reinterprets numbers; it does not convert existing temperatures, limits or offsets.')
table(['Setting','Default','Purpose'],[
('TempCtrlEnable','On','Master controller enable; separate from motor and pump commands.'),
('TempUnits','1','1 = C; 0 = F. Update all related values consistently.'),
('Setpoint','50','Target temperature.'),
('DeadbandHi','5','Upper offset above setpoint.'),
('DeadbandLo','5','Lower offset below setpoint.'),
('HiLimit','90','Upper sensor validity limit; not a measured maximum.'),
('LoLimit','10','Lower sensor validity limit; not a measured minimum.'),
('ErrorTimeout ms','2000','Invalid-reading accumulation threshold. Keep at least two loop periods: 200 ms at default tick.'),
('DeadbandTimeout ms','500','Delay before a qualified request starts a relay.'),
('AtSetPtTimeout ms','500','Delay before active relay releases at setpoint.'),
('Temp2Enable','On','Enable the redundant second probe.'),
('Temp2Offset','0.5','Correction added to sensor 2 for comparison/control.'),
('Temp2Tolerance','4','Allowed qualified disagreement between probes.'),
('TempCompareTimeout ms','5000','Qualified sustained disagreement fault delay.'),
('FilterPoints','4','Moving-average sample count, 1-64; other values use 4.'),
('FeedbackEnable','On','Check relay command against feedback contacts.'),
('RelayFeedbackTimeout ms','1000','Feedback mismatch delay before a fault.')],[172,60,272])
p('Invalid controller setup can produce ConfigFault. Correct the values and apply a valid setup; Reset fault alone does not clear ConfigFault. Preserve a sensible relationship between signal limits, setpoint and deadbands.', 'small')

# 11
page('11. Logs, saved settings and scenarios')
h('Record a run')
steps(['Open Configuration, scroll to <b>Logging</b>, and enable <b>Write CSV + .ncl while running</b>.',
'Run the desired exercise. Disable logging, or close the app normally, to finish and flush the files.',
'Open <b>%LOCALAPPDATA%\\TempSim\\logs</b> using File Explorer. Copy the related files together when sharing a run.'])
table(['File','Contents'],[('.csv','Controller state, probe readings, plant mean, diagnostics and payload.'),('.fixture.csv','Inlet, outlet, outlet-minus-inlet difference, flow and heat powers. Written when fixture mode is logged.'),('.ncl','NI-XNET CAN frame log. Optional for normal simulator use.')],[108,396])
p('The fixture CSV identifies temperature units: 0 = F, 1 = C. Heat is in watts; flow is L/min. Time is simulated seconds. The original controller CSV/CAN message layout does not gain inlet/outlet fields; they are separate model channels.')
h('Saved configuration')
p('Main settings are in <b>%LOCALAPPDATA%\\TempSim\\settings.json</b>; window position is in <b>window.json</b>. Settings save when you close the app normally. Keep a copy of settings.json alongside important runs to record the chosen parameters.')
h('Scripted controller tests')
p('Use Test scenario and Load. The 16 built-in tests include heat-up, cool-down, failover, disagree, relay-feedback, config-fault and two-zones. They use the original plant/profiles to exercise controller rules; inactive fixture graphics are expected. Select Run fixture afterward.')
h('Optional command-line instructions')
p('Open a terminal in the extracted package folder. Run a 600-second fixture simulation:', 'small')
p('.\\TempSim\\TempSim.Cli.exe --scenario fixture<br/>  --seconds 600 --out .\\my-run','code')
p('Enter the wrapped command above on one line. To run every controller test:', 'small')
p('.\\TempSim\\TempSim.Cli.exe --scenario all<br/>  --out .\\controller-checks --quiet','code')
p('A zero exit code means no failed expectations or unpack mismatches. CLI runs are unpaced by default; add --realtime for normal pacing. Add --config FILE to use a JSON setup for a fixture run. The CAN data tab is for inspecting the pack/unpack check; no live CAN hardware is needed.', 'small')

# 12
page('12. Troubleshooting and reference')
table(['What you see','What to do / why it happens'],[
('Old blank bars or fan-like M1/M2 symbols','Check the version line. Close the old app and start the extracted 2.2.1 copy.'),
('Motors run while heater is off','Expected: independent actuation. Use Stop motors; Pause freezes everything.'),
('Fan is not spinning','In automatic mode it waits for the cooler relay. Use manual fan mode if desired.'),
('Timer says Idle or bar is empty','No countdown is active. Short delays are easier to observe at 1x speed.'),
('Outlet is cooler than inlet','Heater warm-up can cause a negative difference. Inspect heater input, actuation heat, flow and fan state.'),
('T1 and T2 differ from outlet or each other','Check sensor offsets, lag, noise, overrides and injected faults. Temp2Offset is a separate controller correction.'),
('Fault immediately returns after reset','Remove the underlying injection or invalid setup. ConfigFault requires valid reinitialization.'),
('App does not start','Extract every file; keep TempSim folder intact. Check 64-bit Windows and share the exact error text.'),
('Want factory settings','Close the app. Rename settings.json and window.json in %LOCALAPPDATA%\\TempSim, then reopen. Keep renamed copies for recovery.')],[178,326])
h('Useful terms')
p('<b>Deadband:</b> interval around setpoint where an idle controller does not request a relay. <b>Feedback:</b> simulated physical relay contact. <b>Accumulator:</b> stored invalid-reading time. <b>Delta T:</b> outlet minus inlet. <b>UUT:</b> unit under test. <b>rpm:</b> revolutions per minute. <b>W/K:</b> heat exchange per degree of temperature difference.', 'small')
h('Model limits and verification')
p('This is an estimated, two-mass model. It does not resolve local hot spots, pipe transport delays, detailed motor mechanics or a complete machine safety interlock. Rotation is deliberately slowed for visibility. The Windows build has 110 controller expectations and 30 physics checks; these establish software behavior, not agreement with a calibrated physical bench. New-model Pi/cRIO execution remains unverified.', 'small')
p('<b>Document basis:</b> TempSim 2.2.1 source and supplied START_HERE.txt, SIMULATOR.md and CHANGELOG.md; TempCtl 3.0.0 and CanTp 1.3.1. Prepared for the current Windows distribution on 20 September 2026. Consult those files and TESTLOG.txt for implementation and verification details.', 'small')

def footer(c,doc):
    c.saveState(); c.setStrokeGray(0); c.setFillGray(0)
    c.setLineWidth(.5); c.line(54,42,558,42)
    c.setFont('Helvetica',8.5); c.drawString(54,28,'TempSim 2.2.1 | User manual | Black-and-white print edition')
    c.drawRightString(558,28,str(doc.page)); c.restoreState()

doc=SimpleDocTemplate(str(OUT),pagesize=letter,rightMargin=54,leftMargin=54,topMargin=43,bottomMargin=55,
    title='TempSim 2.2.1 - User manual and operating instructions',author='TempSim project',pageCompression=1)
doc.build(story,onFirstPage=footer,onLaterPages=footer)
print(OUT)
