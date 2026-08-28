#include "webpage.h"

/* ==========================================================================
 *  PART 1 - the document head and the whole stylesheet.
 *
 *  Dark by default because this is read in a cab, in a workshop, or outdoors
 *  at night, and a white page at 2 a.m. next to a machine is genuinely worse.
 *  The palette is the one the diagnostics view already used.
 * ======================================================================== */
static const char PAGE_1[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>CAN Logger</title>
<style>
:root{
  --bg:#0e1116; --panel:#171c24; --line:#252c38; --txt:#e8edf5; --dim:#8b97a8;
  --ok:#22c55e; --warn:#f59e0b; --bad:#ef4444; --acc:#3b82f6;
  --track:#222b38; --sunk:#0d1219;
  --tt:180ms;                 /* needle travel: set from the poll interval */
}
*{box-sizing:border-box}
/* The UA rule for [hidden] is display:none at zero specificity, so any class
   that sets display beats it - which is exactly what .bar{display:flex} did to
   the customize toolbar. Say it once, loudly, rather than remembering to add
   a [hidden] variant to every component. */
[hidden]{display:none !important}
body{margin:0;background:var(--bg);color:var(--txt);
  font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  padding:14px;padding-bottom:32px;-webkit-text-size-adjust:100%}
h1{font-size:19px;margin:0;letter-spacing:.2px;white-space:nowrap}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px}
#conn{margin-left:auto;font-size:13px;color:var(--dim)}
/* Header controls, on every tab: the frame map, who this logger is, and the
   setup file are none of them a dashboard thing or a sending thing. */
.hbtn{width:auto;margin:0;padding:9px 14px;background:transparent;font-size:13px;
  font-weight:500;border:1px solid var(--line);color:var(--dim)}
.hbtn:hover{border-color:var(--acc);color:var(--txt)}
/* An answered role is worth seeing without reading it, because the wrong one
   quietly fills the wrong half of the frame map into the wrong screen. */
.hbtn.set{border-color:var(--acc);color:var(--txt)}
#rolelist button{width:100%;margin:0 0 8px;text-align:left}
#rolelist button.pri{background:var(--acc);border-color:var(--acc);color:#fff}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px}
.card h2{font-size:12px;letter-spacing:.14em;text-transform:uppercase;
  color:var(--dim);margin:0 0 10px;font-weight:600}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}
.grid5{grid-template-columns:repeat(auto-fit,minmax(215px,1fr))}
.ctl{margin-top:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.sub{font-size:13px;color:var(--dim);margin-top:5px}
.foot{color:var(--dim);font-size:12px;margin-top:12px;text-align:center}
/* A button with no class of its own still has to look like part of this page.
   Without this it fell back to the browser's default chrome - a pale slab -
   which is what "Add an option" and "Send this frame" were. */
button{font:inherit;font-weight:650;border:1px solid var(--line);border-radius:11px;
  padding:14px 22px;background:var(--sunk);color:var(--txt);cursor:pointer;
  width:100%;margin-top:12px;letter-spacing:.02em}
button:hover{border-color:var(--acc)}
button:active{transform:translateY(1px)}
button:disabled{opacity:.4;cursor:not-allowed;transform:none}
.start{background:var(--ok);border-color:var(--ok);color:#fff}
.stop{background:var(--bad);border-color:var(--bad);color:#fff}
.sheetbox > button.pri, .card > button.pri, section button.pri{
  background:var(--acc);border-color:var(--acc);color:#fff}
button.reboot{background:transparent;border:1px solid var(--line);color:var(--dim);
  font-weight:500}
button.reboot:hover{border-color:var(--bad);color:var(--bad)}

/* ---- tabs. Only the visible one is polled, which is most of the reason the
       page costs the logger less than the old single view did. ---- */
.tabs{display:flex;gap:2px;background:var(--sunk);border:1px solid var(--line);
  border-radius:12px;padding:3px}
.tabs button{width:auto;margin:0;padding:8px 14px;background:transparent;
  color:var(--dim);border-radius:9px;font-size:13px}
.tabs button.on{background:var(--panel);color:var(--txt)}
.tabs button .pip{display:inline-block;width:6px;height:6px;border-radius:50%;
  background:var(--bad);margin-left:6px;vertical-align:middle;visibility:hidden}
.tabs button.alert .pip{visibility:visible}

/* ---- the dashboard grid ---------------------------------------------- *
   The column count is the user's, right up to the point where honouring it
   would make every cell unreadable. A four-across layout designed at a desk is
   four illegible slivers on the phone that is actually in the cab, so below
   820px it collapses to two and below 420px to one.                        */
.dgrid{display:grid;gap:10px;grid-template-columns:repeat(var(--cols,4),minmax(0,1fr))}
@media(max-width:820px){.dgrid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:420px){.dgrid{grid-template-columns:1fr}}

.cell{background:var(--panel);border:1px solid var(--line);border-radius:14px;
  padding:10px 8px 10px;display:flex;flex-direction:column;align-items:center;
  min-height:158px;position:relative;overflow:hidden;touch-action:manipulation}
.cell .cname{font-size:10.5px;letter-spacing:.12em;text-transform:uppercase;
  color:var(--dim);font-weight:600;text-align:center;width:100%;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;padding:0 22px}
.cell .gfx{flex:1;min-height:0;width:100%;display:flex;align-items:center;
  justify-content:center;padding:4px 0}
.cell svg{width:100%;height:100%;max-height:118px;display:block;overflow:visible}
.cell .cval{font-size:23px;font-weight:650;font-variant-numeric:tabular-nums;
  line-height:1.05;letter-spacing:-.01em}
.cell .cval.big{font-size:34px}
.cell .cunit{font-size:11.5px;color:var(--dim);margin-top:2px}
.cell.stale{opacity:.42}
.cell.stale .cval{color:var(--dim)}
.v-ok{color:var(--txt)} .v-warn{color:var(--warn)} .v-bad{color:var(--bad)}
.cell.miss{border-color:rgba(245,158,11,.55)}
.cell .why{font-size:11px;color:var(--warn);text-align:center;padding:0 6px}

/* a state pill, for signals whose value is a name rather than a number */
.pill{padding:9px 18px;border-radius:11px;font-size:19px;font-weight:650;
  letter-spacing:.02em;background:var(--track);color:var(--txt);text-align:center;
  max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pill.p-ok{background:rgba(34,197,94,.16);color:var(--ok)}
.pill.p-warn{background:rgba(245,158,11,.16);color:var(--warn)}
.pill.p-bad{background:rgba(239,68,68,.16);color:var(--bad)}
.pill.p-acc{background:rgba(59,130,246,.16);color:var(--acc)}

/* ---- customize mode ---- */
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:12px}
.bar button{width:auto;margin:0;padding:10px 15px;font-size:13.5px;
  background:var(--sunk);border:1px solid var(--line);color:var(--txt)}
.bar button.pri{background:var(--acc);border-color:var(--acc)}
.bar button.warn{color:var(--bad);border-color:rgba(239,68,68,.4)}
.bar .spacer{flex:1}
.step{display:flex;align-items:center;gap:6px;background:var(--sunk);
  border:1px solid var(--line);border-radius:11px;padding:4px 6px}
.step span{font-size:11px;letter-spacing:.1em;text-transform:uppercase;
  color:var(--dim);font-weight:600;padding:0 4px}
.step b{font-variant-numeric:tabular-nums;min-width:16px;text-align:center;
  font-size:14px}
.step button{width:28px;height:28px;padding:0;margin:0;border-radius:8px;
  font-size:16px;line-height:1;background:var(--panel)}

.cell .tools{position:absolute;top:6px;right:6px;display:none;gap:4px;z-index:3}
.edit .cell .tools{display:flex}
.cell .tools button{width:26px;height:26px;padding:0;margin:0;border-radius:8px;
  background:var(--sunk);border:1px solid var(--line);color:var(--dim);
  font-size:13px;font-weight:400;line-height:1}
.cell .tools button:hover{color:var(--txt);border-color:var(--acc)}
.edit .cell{cursor:grab;user-select:none;-webkit-user-select:none}
.edit .cell.slot{touch-action:none}
.cell.empty{border-style:dashed;color:var(--dim);align-items:center;
  justify-content:center;cursor:pointer;font-size:13px;gap:6px}
.cell.empty:hover{border-color:var(--acc);color:var(--txt)}
.cell.lift{opacity:.22}
.cell.over{outline:2px dashed var(--acc);outline-offset:-5px}
#ghost{position:fixed;z-index:60;pointer-events:none;opacity:.92;
  transform:translate(-50%,-50%);box-shadow:0 16px 36px rgba(0,0,0,.55);
  border-color:var(--acc)}
.hint{color:var(--dim);font-size:14px;text-align:center;padding:38px 16px;
  border:1px dashed var(--line);border-radius:14px;line-height:1.7}
.hint b{color:var(--txt);font-weight:650}

/* ---- the cell editor ---- */
.sheet{position:fixed;inset:0;background:rgba(4,7,11,.74);z-index:70;
  display:none;align-items:center;justify-content:center;padding:14px}
.sheet.on{display:flex}
.sheetbox{background:var(--panel);border:1px solid var(--line);border-radius:16px;
  width:min(780px,100%);max-height:94vh;overflow:auto;padding:18px}
.sheetbox h3{margin:0 0 14px;font-size:16px;font-weight:650}
.sheetbox h4{margin:16px 0 8px;font-size:11px;letter-spacing:.13em;
  text-transform:uppercase;color:var(--dim);font-weight:600}
.preview{display:flex;justify-content:center;background:var(--sunk);
  border:1px solid var(--line);border-radius:12px;padding:10px;margin-bottom:4px}
.preview .cell{background:transparent;border:0;min-height:170px;width:220px}
.wpick{display:grid;grid-template-columns:repeat(auto-fill,minmax(76px,1fr));gap:8px}
.wpick button{margin:0;padding:7px 3px 5px;background:var(--sunk);
  border:1px solid var(--line);color:var(--dim);border-radius:10px;font-size:9.5px;
  font-weight:600;letter-spacing:.05em;text-transform:uppercase;width:auto;
  display:flex;flex-direction:column;align-items:center;gap:3px}
.wpick button.on{border-color:var(--acc);color:var(--txt);background:#101826}
.wpick svg{width:100%;height:30px}
.slist{max-height:236px;overflow:auto;border:1px solid var(--line);
  border-radius:11px;background:var(--sunk)}
.slist .row{padding:8px 11px;border-bottom:1px solid #1b212b;cursor:pointer;
  display:flex;gap:10px;align-items:baseline}
.slist .row:last-child{border-bottom:0}
.slist .row:hover{background:#141b26}
.slist .row.on{background:#16233a}
.slist .sn{flex:1;font-size:14px;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
.slist .sm{font-size:11px;color:var(--dim);flex:none}
.slist .sv{font-variant-numeric:tabular-nums;font-size:13px;color:var(--acc);
  flex:none}
.slist .none{padding:14px;color:var(--dim);font-size:13px;text-align:center}
.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));
  gap:10px}
label{display:block;font-size:10.5px;letter-spacing:.1em;text-transform:uppercase;
  color:var(--dim);margin-bottom:5px;font-weight:600}
input,select,textarea{width:100%;background:var(--sunk);border:1px solid var(--line);
  color:var(--txt);border-radius:9px;padding:9px 11px;font:inherit;font-size:14px}
input:focus,select:focus,textarea:focus{outline:0;border-color:var(--acc)}
input[type=checkbox]{width:auto;accent-color:var(--acc)}
.chk{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--dim);
  margin-top:8px}
.chk label{margin:0;text-transform:none;letter-spacing:0;font-size:13px;
  color:var(--txt);font-weight:400}
.acts{display:flex;gap:8px;margin-top:18px;flex-wrap:wrap}
.acts button{width:auto;margin:0;flex:1;min-width:110px;background:var(--sunk);
  border:1px solid var(--line)}
.acts button.pri{background:var(--acc);border-color:var(--acc);flex:2}
.acts button.warn{color:var(--bad);border-color:rgba(239,68,68,.4);flex:0 0 auto}

/* ---- send ----
   The arm control is a button and nothing cleverer. It was a toggle switch to
   begin with, which read as a setting rather than as an action, and a
   press-and-hold after that, which was simply annoying. What it has to be is
   unmissable, deliberate, and separate from the Send buttons themselves - and
   a big button that changes the whole card when it is on does that. */
/* Fixed to the viewport rather than sticky in the flow: nothing moves when it
   appears, which matters when it appears while somebody is reaching for a
   Send button. */
.armstick{position:fixed;left:0;right:0;top:0;z-index:60;
  display:flex;align-items:center;gap:10px;padding:10px 16px;
  background:rgba(12,16,22,.94);border-bottom:1px solid var(--line);
  backdrop-filter:blur(8px);
  transform:translateY(-101%);transition:transform .18s ease-out}
.armstick.on{transform:none}
.armstick b{font-size:13px;letter-spacing:.1em}
.armstick .sub{margin:0;font-size:12px}
.armstick button{width:auto;margin:0;padding:9px 16px;font-size:13px}
.armstick.live{border-bottom-color:var(--bad);
  background:linear-gradient(180deg,rgba(239,68,68,.16),rgba(12,16,22,.94))}
.armstick.live b{color:var(--bad)}
.armstick.live button{background:transparent;border:1px solid var(--bad);
  color:var(--bad)}
.armstick .dot{background:var(--dim)}
.armstick.live .dot{background:var(--bad)}
@media(max-width:560px){.armstick .sub{display:none}}

#armcard{transition:border-color .25s,background .25s}
#armcard.on{border-color:var(--bad);background:linear-gradient(
  180deg,rgba(239,68,68,.10),rgba(239,68,68,.02))}
.armhead{display:flex;align-items:center;gap:14px;flex-wrap:wrap}
.armhead .txt{flex:1;min-width:150px}
.armstate{font-size:21px;font-weight:700;letter-spacing:.02em;line-height:1.2}
#armcard.on .armstate{color:var(--bad)}
#armbtn{width:auto;margin:0;padding:15px 30px;background:var(--acc);
  font-size:15px;white-space:nowrap}
#armcard.on #armbtn{background:transparent;border:1px solid var(--bad);
  color:var(--bad)}
.armbar{height:4px;border-radius:2px;background:var(--sunk);margin-top:14px;
  overflow:hidden;display:none}
#armcard.on .armbar{display:block}
.armbar span{display:block;height:100%;background:var(--bad);width:100%;
  transition:width 1s linear}

.sendrow{display:flex;align-items:center;gap:10px;padding:13px 0;
  border-bottom:1px solid #1b212b;flex-wrap:wrap}
.sendrow:last-child{border-bottom:0}

/* A frame that only means anything whole, drawn as one thing. The box is the
   point: it is what tells somebody that pressing Send moves all of these. */
.sendgroup{border:1px solid var(--line);border-radius:12px;padding:4px 14px 14px;
  margin:12px 0;background:var(--sunk)}
.sendgroup .ghead{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;
  padding:11px 0 4px}
.sendgroup .ghead b{font-size:13px;letter-spacing:.08em;text-transform:uppercase}
.sendgroup .ghead span{font-size:12px;color:var(--dim)}
/* The group's own Remove, in the editor. Compact and pushed to the right, like
   the one on each card - left at full width it reads as the group's main
   action, which on a screen full of values it very much is not. */
.sendgroup .ghead button{width:auto;margin:0 0 0 auto;padding:7px 12px;
  font-size:12px;color:var(--bad);border-color:transparent;background:transparent}
.sendgroup .ghead button:hover{border-color:var(--bad)}
/* Editor groups only: let the explanation take the space it needs so the button
   stays on the first line with the name, instead of being pushed to a third. */
.txgroup .ghead{align-items:center}
.txgroup .ghead span{flex:1 1 240px}
.sendgroup .sendrow{border-bottom:1px dashed #1b212b}
.sendgroup .sendrow:last-of-type{border-bottom:0}
.sendrow .lbl em{font-style:normal;color:var(--acc)}
.sendrow .lbl{flex:1 1 150px;min-width:130px;font-size:14.5px;font-weight:600}
.sendrow .lbl small{display:block;font-weight:400;font-size:11.5px;
  color:var(--dim);font-variant-numeric:tabular-nums}
.sendrow .inp{flex:1 1 210px;display:flex;align-items:center;gap:9px;min-width:0}
.sendrow input[type=number]{width:110px;flex:none;text-align:right;
  font-variant-numeric:tabular-nums}
.sendrow input[type=range]{flex:1;min-width:110px;accent-color:var(--acc);
  padding:0;background:transparent;border:0}
.sendrow select{flex:1;min-width:120px}
.sendrow .u{font-size:12px;color:var(--dim);min-width:30px}
.sendrow .rv{font-variant-numeric:tabular-nums;font-weight:650;min-width:64px;
  text-align:right;font-size:15px}
.sendrow button{width:auto;margin:0;padding:11px 20px;font-size:13.5px;
  background:var(--acc);flex:none}
.sendrow button.cyc{background:var(--sunk);border:1px solid var(--line);
  color:var(--dim);padding:11px 15px}
.sendrow button.cyc.on{background:rgba(239,68,68,.18);border-color:var(--bad);
  color:var(--bad)}

/* two-state input: two buttons, not a switch - the labels stay visible */
.seg{display:flex;gap:3px;background:var(--sunk);border:1px solid var(--line);
  border-radius:10px;padding:3px;flex:1;min-width:130px}
.seg button{flex:1;width:auto;margin:0;padding:9px 10px;background:transparent;
  color:var(--dim);border-radius:7px;font-size:13px}
.seg button.on{background:var(--acc);color:#fff}

/* the option-list editor in the set-up sheet */
.opts{display:flex;flex-direction:column;gap:6px;margin-top:8px}
.optrow{display:flex;gap:6px;align-items:center}
.optrow input:first-child{width:92px;flex:none}
.optrow button{width:34px;flex:none;margin:0;padding:9px 0;background:var(--sunk);
  border:1px solid var(--line);color:var(--dim);font-weight:400}
.txcard{background:var(--sunk);border:1px solid var(--line);border-radius:12px;
  padding:14px;margin-bottom:10px}
.txcard .head{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.txcard .head b{flex:1;font-size:14px}
.txcard .head button{width:auto;margin:0;padding:7px 12px;font-size:12px;
  background:transparent;border:1px solid var(--line);color:var(--bad)}
.sendrow{display:flex;align-items:center;gap:10px;padding:12px 0;
  border-bottom:1px solid #1b212b;flex-wrap:wrap}
.sendrow:last-child{border-bottom:0}
.sendrow .lbl{flex:1;min-width:130px;font-size:14.5px;font-weight:600}
.sendrow .lbl small{display:block;font-weight:400;font-size:11.5px;
  color:var(--dim);font-variant-numeric:tabular-nums}
.sendrow input{width:118px;flex:none;text-align:right;
  font-variant-numeric:tabular-nums}
.sendrow .u{font-size:12px;color:var(--dim);min-width:34px}
.sendrow button{width:auto;margin:0;padding:10px 18px;font-size:13.5px;
  background:var(--acc)}
.sendrow button.cyc{background:var(--sunk);border:1px solid var(--line);
  color:var(--dim)}
.sendrow button.cyc.on{background:rgba(239,68,68,.18);border-color:var(--bad);
  color:var(--bad)}
.warnbox{background:rgba(245,158,11,.09);border:1px solid rgba(245,158,11,.35);
  border-radius:11px;padding:12px 14px;font-size:13px;line-height:1.6;
  color:#f5d9a8;margin-bottom:14px}
.warnbox b{color:var(--warn)}

/* ---- toasts ---- */
#toasts{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);z-index:80;
  display:flex;flex-direction:column;gap:8px;align-items:center;
  pointer-events:none;width:min(460px,92vw)}
.toast{background:var(--panel);border:1px solid var(--line);
  border-left:3px solid var(--acc);border-radius:10px;padding:10px 15px;
  font-size:13px;box-shadow:0 10px 28px rgba(0,0,0,.5);width:100%;
  animation:rise .18s ease}
@keyframes rise{from{opacity:0;transform:translateY(8px)}}
.toast.bad{border-left-color:var(--bad)}
.toast.ok{border-left-color:var(--ok)}
.toast b{display:block;font-size:12px;color:var(--dim);font-weight:600;
  letter-spacing:.08em;text-transform:uppercase;margin-bottom:2px}

/* ---- diagnostics, unchanged from the view this page grew out of ---- */
.meter{height:6px;border-radius:3px;background:var(--sunk);margin-top:12px;
  overflow:hidden}
.meter span{display:block;height:100%;width:0;background:var(--ok);
  transition:width .4s ease,background .4s ease}
.state{display:flex;align-items:center;gap:12px}
.dot{width:18px;height:18px;border-radius:50%;flex:none;background:var(--dim);
  box-shadow:0 0 0 4px rgba(255,255,255,.05)}
.dot.ok{background:var(--ok);box-shadow:0 0 0 4px rgba(34,197,94,.18)}
.dot.warn{background:var(--warn);box-shadow:0 0 0 4px rgba(245,158,11,.18)}
.dot.bad{background:var(--bad);box-shadow:0 0 0 4px rgba(239,68,68,.18)}
.dot.rec{animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{50%{opacity:.35}}
.big{font-size:23px;font-weight:650;line-height:1.15}
.scroll{overflow-x:auto}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
th{font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim);
  text-align:left;font-weight:600;padding:6px 8px;border-bottom:1px solid var(--line)}
th.num,td.num{text-align:right;font-variant-numeric:tabular-nums}
table.sig{table-layout:fixed}
table.sig th.c1,table.sig td.c1{width:30%}
table.sig th.c2,table.sig td.c2{width:30%}
table.sig th.c3,table.sig td.c3{width:22%}
table.sig th.c4,table.sig td.c4{width:18%;padding-left:14px}
td.ell{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
td{padding:6px 8px;border-bottom:1px solid #1b212b;font-size:14px}
td.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
tr.un td{color:var(--dim)}
#term{background:#080b0f;border:1px solid var(--line);border-radius:12px;
  margin-top:12px;height:290px;overflow:auto;padding:11px 13px;
  font:12.5px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  color:#a8b6c8;white-space:pre-wrap;word-break:break-word}
#term div.W{color:var(--warn)} #term div.E{color:var(--bad)}
#term div.I{color:#cfe0f5}
</style></head><body>
)HTML";

/* ==========================================================================
 *  PART 2 - the markup.
 *
 *  Four views behind one set of tabs, arranged around WHEN each is used:
 *
 *    Dashboard  on the machine. Is the logger healthy, what is the bus doing
 *               in the terms this operator chose, and start/stop.
 *    Bus        when something is wrong or the frame map is being worked out:
 *               the raw identifiers and every decoded signal as text.
 *    Send       writing a value back.
 *    Log        what the logger has been saying.
 *
 *  The Dashboard deliberately carries the health tiles and the recording
 *  controls as well as the custom grid, because those three things are what
 *  somebody standing at the machine needs, and making them switch tabs to find
 *  out whether the card is still being written to would be absurd.
 * ======================================================================== */
static const char PAGE_2[] PROGMEM = R"HTML(
<header>
  <h1>CAN Logger</h1>
  <div class="tabs" id="tabs">
    <button data-tab="dash" class="on">Dashboard</button>
    <button data-tab="bus">Bus</button>
    <button data-tab="send">Send</button>
    <button data-tab="log">Log</button>
  </div>
  <button id="dbcbtn"   class="hbtn">Frame map</button>
  <button id="rolebtn"  class="hbtn">Role: none</button>
  <button id="setupbtn" class="hbtn">Setup file</button>
  <span id="conn">connecting...</span>
</header>

<!-- ==================== DASHBOARD ==================== -->
<section id="v_dash">

  <!-- health first: this is what a logger is judged on -->
  <div class="grid grid5">
    <div class="card"><h2>SD Card</h2>
      <div class="state"><span class="dot" id="d_sd"></span>
        <div><div class="big" id="t_sd">--</div><div class="sub" id="s_sd"></div></div></div>
    </div>
    <div class="card"><h2>Bus</h2>
      <div class="state"><span class="dot" id="d_can"></span>
        <div><div class="big" id="t_can">--</div><div class="sub" id="s_can"></div></div></div>
    </div>
    <div class="card"><h2>Interrupt Path</h2>
      <div class="state"><span class="dot" id="d_irq"></span>
        <div><div class="big" id="t_irq">--</div><div class="sub" id="s_irq"></div></div></div>
    </div>
    <div class="card"><h2>Data Integrity</h2>
      <div class="state"><span class="dot" id="d_lost"></span>
        <div><div class="big" id="t_lost">--</div><div class="sub" id="s_lost"></div></div></div>
    </div>
    <div class="card"><h2>CAN Bus Load</h2>
      <div class="state"><span class="dot" id="d_load"></span>
        <div><div class="big" id="t_load">--</div><div class="sub" id="s_load"></div></div></div>
      <div class="meter"><span id="loadbar"></span></div>
    </div>
  </div>

  <!-- then whatever this operator decided matters -->
  <div class="bar" id="viewbar" style="margin-top:16px">
    <button id="customize">Customize dashboard</button>
    <div class="spacer"></div>
    <span class="sub" id="dashnote">&nbsp;</span>
  </div>

  <div class="bar" id="editbar" hidden style="margin-top:16px">
    <div class="step">
      <span>Columns</span>
      <button id="colminus">&minus;</button><b id="colnum">4</b><button id="colplus">+</button>
    </div>
    <div class="step">
      <span>Rows</span>
      <button id="rowminus">&minus;</button><b id="rownum">2</b><button id="rowplus">+</button>
    </div>
    <button id="fillmap">Fill from frame map</button>
    <button id="fillbus">Fill from bus</button>
    <button id="clearcfg" class="warn">Clear all</button>
    <div class="spacer"></div>
    <button id="donedit" class="pri">Done</button>
  </div>

  <div class="dgrid" id="grid"></div>

  <div class="hint" id="emptyhint" hidden>
    No dashboard has been set up on this logger yet.<br>
    Press <b>Customize dashboard</b>, then tap a cell to choose a signal and how
    to draw it &mdash; or press <b>Fill from frame map</b> to lay out everything
    the loaded <code>.dbc</code> describes, which is how this is normally done at
    a desk before going out.<br>
    <span style="font-size:12.5px">What you build here is written to
    <b>/dash.cfg</b> on the SD card, so it is already there the next time this
    logger starts.</span>
  </div>

  <!-- and the two controls that are needed with cold hands -->
  <div class="grid ctl">
    <div class="card"><h2>Recording</h2>
      <div class="state"><span class="dot" id="d_rec"></span>
        <div><div class="big" id="t_rec">--</div><div class="sub" id="s_rec"></div></div></div>
      <button id="btn" class="start">START</button>
    </div>
    <div class="card"><h2>Logger</h2>
      <div class="state"><span class="dot ok"></span>
        <div><div class="big">RUNNING</div><div class="sub" id="s_up">&nbsp;</div></div></div>
      <button class="reboot" id="rebootbtn">RESTART</button>
    </div>
  </div>
</section>

<!-- ==================== BUS ==================== -->
<section id="v_bus" hidden>
  <div class="card">
    <h2>Live Signals</h2>
    <div class="scroll">
      <table class="sig"><thead><tr>
        <th class="c1">Message</th><th class="c2">Signal</th>
        <th class="c3 num">Value</th><th class="c4">Unit</th>
      </tr></thead><tbody id="sigs"></tbody></table>
    </div>
    <div class="sub" id="s_sigs">&nbsp;</div>
  </div>

  <div class="card" style="margin-top:12px">
    <h2>Identifiers on the wire</h2>
    <div class="scroll">
      <table><thead><tr>
        <th>ID</th><th>Last payload</th><th>Frames</th><th>Rate</th><th>Mapped</th>
      </tr></thead><tbody id="ids"></tbody></table>
    </div>
    <div class="sub" id="s_ids">&nbsp;</div>
  </div>
</section>

<!-- ==================== SEND ==================== -->
<section id="v_send" hidden>
  <!-- The arm control again, pinned to the top of the screen once the real one
       has scrolled away. With thirty values set up, ARM and DISARM must not be
       a scroll to the top of the page. -->
  <div class="armstick" id="armstick">
    <span class="dot" id="stickdot"></span>
    <b id="stickstate">NOT ARMED</b>
    <span class="sub" id="sticksub"></span>
    <div class="spacer"></div>
    <button id="stickbtn">ARM TRANSMIT</button>
  </div>

  <div class="warnbox">
    <b>This writes to the bus.</b> A frame sent to a live controller can move
    hydraulics, release a brake or enable a drive &mdash; the logger sends what
    it is told and cannot know which. Arm it only when you know what is on the
    other end, and what the value you are about to send does.
  </div>

  <div class="card" id="armcard">
    <h2>Permission</h2>
    <div class="armhead">
      <div class="txt">
        <div class="armstate" id="armstate">NOT ARMED</div>
        <div class="sub" id="armsub">Send is disabled</div>
      </div>
      <button id="armbtn">ARM TRANSMIT</button>
    </div>
    <div class="armbar"><span id="armfill"></span></div>
  </div>

  <!-- Where "Customize dashboard" sits on the other tab: above the thing it
       changes, not buried under it. -->
  <div class="bar" style="margin-top:14px">
    <button id="editsend">Set up sendable values</button>
    <div class="spacer"></div>
    <span class="sub" id="sendcount">&nbsp;</span>
  </div>

  <div class="card" id="savedcard" style="margin-top:12px" hidden>
    <h2>Saved values</h2>
    <div id="sendlist"></div>
    <div class="sub" id="sendnote">&nbsp;</div>
  </div>

  <div class="card" style="margin-top:12px">
    <h2>One-off frame</h2>
    <div class="fields">
      <div><label for="rawid">Identifier</label>
        <input id="rawid" placeholder="0x600" autocomplete="off"></div>
      <div style="grid-column:span 2"><label for="rawdata">Payload, hex</label>
        <input id="rawdata" placeholder="2F10200001000000" autocomplete="off"></div>
    </div>
    <button id="rawsend" class="pri">Send this frame</button>
    <div class="sub">Up to eight bytes, exactly as typed. This is the way out
      when a value is not in the frame map at all &mdash; nothing about it is
      remembered.</div>
  </div>
</section>

<!-- ==================== LOG ==================== -->
<section id="v_log" hidden>
  <div class="card"><h2>Live Log</h2><div id="term"></div></div>
</section>

<div class="foot" id="foot">&nbsp;</div>
<div id="toasts"></div>

<!-- the dashboard cell editor -->
<div class="sheet" id="sheet">
  <div class="sheetbox">
    <h3 id="sheettitle">Cell</h3>
    <div class="preview"><div class="cell" id="prevcell"></div></div>

    <h4>Signal</h4>
    <input id="sfilter" placeholder="Search messages and signals" autocomplete="off">
    <div class="slist" id="slist" style="margin-top:8px"></div>

    <h4>How to draw it</h4>
    <div class="wpick" id="wpick"></div>

    <h4>Details</h4>
    <div class="fields">
      <div><label for="f_label">Title</label><input id="f_label" autocomplete="off"></div>
      <div id="fw_unit"><label for="f_unit">Unit</label><input id="f_unit" autocomplete="off"></div>
      <div id="fw_dec"><label for="f_dec">Decimals</label><input id="f_dec" type="number" min="0" max="6"></div>
      <div id="fw_lo"><label for="f_lo">Minimum</label><input id="f_lo" type="number" step="any"></div>
      <div id="fw_hi"><label for="f_hi">Maximum</label><input id="f_hi" type="number" step="any"></div>
    </div>
    <div class="hint" id="statenote" hidden style="margin-top:10px"></div>

    <h4 id="h_thresh">Colour thresholds &mdash; optional</h4>
    <div class="fields" id="fw_thresh">
      <div><label for="f_warn">Amber past</label><input id="f_warn" type="number" step="any" placeholder="none"></div>
      <div><label for="f_crit">Red past</label><input id="f_crit" type="number" step="any" placeholder="none"></div>
    </div>
    <div class="chk" id="fw_lowbad">
      <input type="checkbox" id="f_lowbad"><label for="f_lowbad">Low values are the bad ones (a level or a pressure that must not drop)</label>
    </div>

    <div class="acts">
      <button id="e_del" class="warn">Remove</button>
      <button id="e_cancel">Cancel</button>
      <button id="e_ok" class="pri">Apply</button>
    </div>
  </div>
</div>

<!-- setting up the values that can be sent -->
<div class="sheet" id="txsheet">
  <div class="sheetbox">
    <h3>Values that can be sent</h3>
    <div class="sub" style="margin-bottom:14px">Set these up here, before you go
      out. Choose the signal from the frame map, then decide how the value is
      picked &mdash; a list of the four tyre sizes your fleet actually uses is a
      far better thing to hand somebody standing next to a running machine than
      an empty number box.</div>
    <div class="card" style="margin-bottom:14px">
      <h2>Fill from the frame map</h2>
      <div class="sub" style="margin:0 0 4px">Most machines take their settings
        from one message. Choose it, and every signal in it becomes a value you
        can send, with the input guessed from the file &mdash; a signal the
        <code>.dbc</code> gives names to becomes a list, a one-bit signal becomes
        two states, a signal with a declared range becomes a slider, anything
        else a number box. Change any of them below.</div>
      <select id="txfillmsg"></select>
      <button id="txfill">Add every signal in this message</button>
    </div>

    <div id="txeditlist"></div>
    <div class="acts">
      <button id="tx_add">Add a value</button>
      <button id="tx_cancel">Cancel</button>
      <button id="tx_ok" class="pri">Apply</button>
    </div>
  </div>
</div>

<!-- the setup file: one sheet, reachable from every tab -->
<div class="sheet" id="cfgsheet">
  <div class="sheetbox">
    <h3>Setup file</h3>
    <div class="sub" style="margin-bottom:14px">Everything you customize on this
      logger lives in one file, <b>/dash.cfg</b> on the SD card &mdash; the
      dashboard cells <i>and</i> the values that can be sent. There is nothing
      else to back up, and nothing else to copy onto the next machine.</div>

    <div class="card"><h2>What is on this logger now</h2>
      <div class="big" id="cfgsummary">&mdash;</div>
      <div class="sub" id="cfgbytes">&nbsp;</div>
    </div>

    <h4>Export</h4>
    <div class="sub">Downloads the file <b>to the phone or laptop you are
      holding</b>, into its downloads folder. Nothing on the logger changes and
      no card is written, so this is safe to press at any time &mdash; including
      in the middle of a recording.</div>
    <button class="cfgexport pri">Export to this device</button>

    <h4>Import</h4>
    <div class="sub">Replaces <b>both</b> halves &mdash; the dashboard and the
      sendable values &mdash; on the SD card and in the logger&rsquo;s own
      memory. Export first if you want the current one back.</div>
    <button class="cfgimport">Import from this device</button>

    <div class="acts">
      <button id="cfg_close" class="pri">Close</button>
    </div>
  </div>
</div>

<!-- which node of the frame map this logger is, asked FIRST because it is
     what both Fill buttons need in order to be useful -->
<div class="sheet" id="rolesheet">
  <div class="sheetbox">
    <h3>Which of these is this logger?</h3>
    <div class="sub" style="margin-bottom:14px">A <code>.dbc</code> says who
      <b>sends</b> each message, but not which of those nodes is the box you are
      holding &mdash; and that is the whole difference between a reading and a
      command. Answer it and <b>Fill</b> puts what your node sends on the
      <b>Send</b> tab and everything else on the <b>dashboard</b>.
      <br><br>Recording never changes: every frame that arrives is logged
      whoever the file says sends it.</div>

    <div id="rolelist"></div>
    <div class="sub" id="rolenote">&nbsp;</div>

    <div class="acts">
      <button id="role_close" class="pri">Done</button>
    </div>
  </div>
</div>

<input type="file" id="filepick" accept=".cfg,.txt,text/plain" hidden>
<input type="file" id="dbcpick" accept=".dbc,text/plain" hidden>
)HTML";

/* ==========================================================================
 *  PART 3 - the widget engine, the layout format, and polling.
 *
 *  The widgets are hand-drawn SVG because the page has to work from a hotspot
 *  with no route to the internet, which rules out every gauge library there is.
 *  They are built ONCE, when a cell is created, and afterwards an update only
 *  moves a needle and rewrites one string - no innerHTML, no re-layout, which
 *  is what makes five updates a second cost nothing on the browser side.
 *
 *  On the logger's side the cost is one request carrying only the cells that
 *  exist, and only for the tab that is actually on screen. A backgrounded
 *  browser tab polls nothing at all.
 * ======================================================================== */
static const char PAGE_3[] PROGMEM = R"HTML(<script>
"use strict";
var NS = 'http://www.w3.org/2000/svg';
function q(id){ return document.getElementById(id); }
function el(tag, cls, txt){
  var e = document.createElement(tag);
  if(cls) e.className = cls;
  if(txt !== undefined) e.textContent = txt;
  return e;
}
function sv(tag, attrs){
  var e = document.createElementNS(NS, tag), k;
  for(k in attrs) e.setAttribute(k, attrs[k]);
  return e;
}
function esc(s){
  return String(s).replace(/[&<>]/g, function(c){
    return c === '&' ? '&amp;' : (c === '<' ? '&lt;' : '&gt;');
  });
}
function hms(s){
  var h = Math.floor(s/3600), m = Math.floor(s/60)%60, x = s%60;
  return (h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(x<10?'0':'')+x;
}

/* ---------------------------------------------------------------------------
 *  The layout, in the same text format the card holds.
 *
 *  Parsed and written here as well as on the logger, deliberately: it means
 *  Export is literally what the device stores, Import is the same bytes going
 *  back, and there is no second representation that could disagree with the
 *  first. The device re-serialises whatever it receives, so its writer stays
 *  the one that decides the canonical form.
 * -------------------------------------------------------------------------*/
var CFG = { cols:4, rows:2, poll:200, role:'', cells:{}, tx:{} };
/* How many cells this firmware can store. Told to us by /api/dash rather than
   written down here, so the two cannot drift apart. */
var MAXCELLS = 48, MAXSEND = 32;
var DBC = { loaded:0, m:[] };     /* the frame map, fetched when the editor opens */
var GEN = 0;

function cellsCount(){ return Math.min(CFG.cols * CFG.rows, MAXCELLS); }

/* Slots are one flat list drawn left to right, top to bottom, so taking a cell
   out of the middle leaves a hole in that list. Close it by pulling everything
   after it back one place - which is what "shift the ones in front up" means
   on a grid. Gaps BEFORE the removed slot are left alone: those are ones
   somebody made on purpose by dragging, and closing them would rearrange a
   layout the operator had already settled. */
function removeCell(slot){
  delete CFG.cells[slot];
  Object.keys(CFG.cells).map(Number).sort(function(a, b){ return a - b; })
    .forEach(function(k){
      if(k > slot){ CFG.cells[k - 1] = CFG.cells[k]; delete CFG.cells[k]; }
    });
}

/* Splits a line into key=value pairs, honouring quotes so a title can contain
 * spaces. Mirrors nextPair() in dash.cpp. */
function pairs(rest){
  var out = {}, i = 0, n = rest.length;
  while(i < n){
    while(i < n && /\s/.test(rest[i])) i++;
    if(i >= n) break;
    var k = '';
    while(i < n && rest[i] !== '=' && !/\s/.test(rest[i])) k += rest[i++];
    var v = '';
    if(rest[i] === '='){
      i++;
      if(rest[i] === '"'){
        i++;
        while(i < n && rest[i] !== '"') v += rest[i++];
        if(rest[i] === '"') i++;
      } else {
        while(i < n && !/\s/.test(rest[i])) v += rest[i++];
      }
    }
    if(k) out[k] = v;
  }
  return out;
}
function numOr(v, dflt){
  if(v === undefined || v === '') return dflt;
  var f = parseFloat(v);
  return isFinite(f) ? f : dflt;
}

function parseCfg(text){
  var c = { cols:4, rows:2, poll:200, role:'', cells:{}, tx:{} };
  text.split(/\r?\n/).forEach(function(line){
    line = line.replace(/^\s+/, '');
    if(!line || line[0] === '#') return;
    var sp = line.indexOf(' ');
    if(sp < 0) return;
    var kw = line.slice(0, sp), rest = line.slice(sp + 1);

    if(kw === 'grid'){
      var g = rest.trim().split(/\s+/);
      c.cols = Math.max(1, Math.min(6, parseInt(g[0], 10) || 4));
      c.rows = Math.max(1, Math.min(8, parseInt(g[1], 10) || 2));
    } else if(kw === 'poll'){
      c.poll = Math.max(100, Math.min(5000, parseInt(rest, 10) || 200));
    } else if(kw === 'role' || kw === 'node'){
      /* 'node' is what this was called in an earlier version. Reading both
         means a setup file from any of them keeps its answer. */
      c.role = rest.trim().replace(/^"|"$/g, '');
    } else if(kw === 'cell' || kw === 'send'){
      var sp2 = rest.indexOf(' ');
      if(sp2 < 0) return;
      var idx = parseInt(rest.slice(0, sp2), 10);
      var p = pairs(rest.slice(sp2 + 1));
      if(!isFinite(idx)) return;

      if(kw === 'cell'){
        if(!p.sig) return;
        c.cells[idx] = {
          w: p.widget || 'number', sig: p.sig,
          label: p.label || '', unit: p.unit || '',
          lo: numOr(p.lo, 0), hi: numOr(p.hi, 100),
          dec: p.dec === undefined ? null : parseInt(p.dec, 10),
          warn: p.warn === undefined ? null : numOr(p.warn, null),
          crit: p.crit === undefined ? null : numOr(p.crit, null),
          lowbad: p.lowbad === '1' ? 1 : 0
        };
      } else {
        if(!p.label) return;
        c.tx[idx] = {
          label: p.label, sig: p.sig || '', unit: p.unit || '',
          lo: numOr(p.lo, 0), hi: numOr(p.hi, 0),
          step: numOr(p.step, 1), preset: numOr(p.preset, 0),
          dec: p.dec === undefined ? null : parseInt(p.dec, 10),
          style: p.style || 'number', choices: p.choices || '',
          id: p.id || '', data: p.data || '', ext: p.ext === '1' ? 1 : 0,
          cyclic: numOr(p.cyclic, 0), group: numOr(p.group, 0),
          mux: p.mux === '1' ? 1 : 0,
          raw: p.id !== undefined || p.data !== undefined
        };
      }
    }
  });
  return c;
}

function quote(v){
  v = String(v === undefined || v === null ? '' : v).replace(/["\n]/g, '');
  return (v === '' || /\s/.test(v)) ? '"' + v + '"' : v;
}
function trimNum(v){
  var s = (Math.round(v * 1000) / 1000).toFixed(3);
  return s.replace(/\.?0+$/, '') || '0';
}

function dumpCfg(){
  var out = ['version 1', 'grid ' + CFG.cols + ' ' + CFG.rows, 'poll ' + CFG.poll];
  if(CFG.role) out.push('role ' + quote(CFG.role));
  Object.keys(CFG.cells).map(Number).sort(function(a,b){return a-b;}).forEach(function(i){
    var c = CFG.cells[i];
    if(!c || !c.sig) return;
    var s = 'cell ' + i + ' widget=' + c.w + ' sig=' + quote(c.sig);
    if(c.label) s += ' label=' + quote(c.label);
    if(c.unit)  s += ' unit=' + quote(c.unit);
    s += ' lo=' + trimNum(c.lo) + ' hi=' + trimNum(c.hi);
    if(c.dec !== null && c.dec !== undefined && isFinite(c.dec)) s += ' dec=' + c.dec;
    if(c.warn !== null && c.warn !== undefined) s += ' warn=' + trimNum(c.warn);
    if(c.crit !== null && c.crit !== undefined) s += ' crit=' + trimNum(c.crit);
    if(c.lowbad) s += ' lowbad=1';
    out.push(s);
  });
  Object.keys(CFG.tx).map(Number).sort(function(a,b){return a-b;}).forEach(function(i){
    var t = CFG.tx[i];
    if(!t || !t.label) return;
    var s = 'send ' + i + ' label=' + quote(t.label);
    if(t.raw){
      s += ' id=' + (t.id || '0x000');
      if(t.ext) s += ' ext=1';
      s += ' data=' + (t.data || '');
    } else {
      if(!t.sig) return;
      s += ' sig=' + quote(t.sig);
      if(t.unit) s += ' unit=' + quote(t.unit);
      s += ' lo=' + trimNum(t.lo) + ' hi=' + trimNum(t.hi) +
           ' step=' + trimNum(t.step) + ' preset=' + trimNum(t.preset);
      if(t.dec !== null && t.dec !== undefined && isFinite(t.dec)) s += ' dec=' + t.dec;
      s += ' style=' + (t.style || 'number');
      if(t.choices) s += ' choices=' + quote(t.choices);
    }
    if(t.group)  s += ' group=' + Math.round(t.group);
    if(t.mux)    s += ' mux=1';
    if(t.cyclic) s += ' cyclic=' + Math.round(t.cyclic);
    out.push(s);
  });
  return out.join('\n') + '\n';
}

/* ---------------------------------------------------------------------------
 *  Drawing
 * -------------------------------------------------------------------------*/
function clamp01(x){ return x < 0 ? 0 : (x > 1 ? 1 : x); }
function frac(c, v){
  var span = c.hi - c.lo;
  return span > 0 ? clamp01((v - c.lo) / span) : 0;
}
function decOf(c){
  if(c.dec !== null && c.dec !== undefined && isFinite(c.dec)) return c.dec;
  var span = Math.abs(c.hi - c.lo);
  return span >= 200 ? 0 : (span >= 20 ? 1 : 2);
}
function fmtVal(v, d){
  if(!isFinite(v)) return '--';
  return v.toFixed(d);
}
/* Tick labels have to fit in nine pixels of a dial, so they lose the decimals
   the readout keeps. */
function tickLabel(v){
  var a = Math.abs(v);
  if(a >= 1000) return (v / 1000).toFixed(a >= 10000 ? 0 : 1) + 'k';
  if(a >= 10 || v === 0) return v.toFixed(0);
  if(a >= 1) return v.toFixed(1);
  return v.toFixed(2);
}

/* Where the coloured bands sit, in value space. With lowbad set the dangerous
   end is the bottom - a hydraulic level or a pressure that must not drop - and
   the bands run the other way. */
function zoneSpans(c){
  var out = [];
  if(c.lowbad){
    if(c.warn !== null && c.warn !== undefined) out.push({a:c.lo, b:c.warn, k:'warn'});
    if(c.crit !== null && c.crit !== undefined) out.push({a:c.lo, b:c.crit, k:'bad'});
  } else {
    if(c.warn !== null && c.warn !== undefined) out.push({a:c.warn, b:c.hi, k:'warn'});
    if(c.crit !== null && c.crit !== undefined) out.push({a:c.crit, b:c.hi, k:'bad'});
  }
  return out;
}
function zoneOf(c, v){
  if(!isFinite(v)) return 'ok';
  var w = c.warn, r = c.crit;
  if(c.lowbad){
    if(r !== null && r !== undefined && v <= r) return 'bad';
    if(w !== null && w !== undefined && v <= w) return 'warn';
  } else {
    if(r !== null && r !== undefined && v >= r) return 'bad';
    if(w !== null && w !== undefined && v >= w) return 'warn';
  }
  return 'ok';
}
function zoneColour(k){
  return k === 'bad' ? 'var(--bad)' : (k === 'warn' ? 'var(--warn)' : 'var(--acc)');
}

function pol(cx, cy, r, deg){
  var a = (deg - 90) * Math.PI / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}
function arcp(cx, cy, r, a0, a1){
  var p = pol(cx, cy, r, a0), s = pol(cx, cy, r, a1);
  return 'M' + p[0].toFixed(2) + ' ' + p[1].toFixed(2) +
         'A' + r + ' ' + r + ' 0 ' + (Math.abs(a1 - a0) > 180 ? 1 : 0) + ' ' +
         (a1 > a0 ? 1 : 0) + ' ' + s[0].toFixed(2) + ' ' + s[1].toFixed(2);
}
function spin(node, cx, cy){
  node.setAttribute('style', 'transform-box:view-box;transform-origin:' +
    cx + 'px ' + cy + 'px;transition:transform var(--tt) linear');
}

/* A dial: the shape behind the speedometer, the half-gauge and the centre-zero
   angle indicator. They differ only in how far they sweep and where the value
   arc starts from. */
function dial(c, o){
  var g = sv('svg', {viewBox:o.vb, preserveAspectRatio:'xMidYMid meet'});
  var a0 = o.a0, a1 = o.a1, span = a1 - a0;
  function ang(v){ return a0 + span * frac(c, v); }

  g.appendChild(sv('path', {d:arcp(o.cx,o.cy,o.r,a0,a1), fill:'none',
    stroke:'var(--track)', 'stroke-width':9, 'stroke-linecap':'round'}));

  zoneSpans(c).forEach(function(z){
    var s = ang(z.a), e = ang(z.b);
    if(Math.abs(e - s) < 0.5) return;
    g.appendChild(sv('path', {d:arcp(o.cx,o.cy,o.r,Math.min(s,e),Math.max(s,e)),
      fill:'none', stroke:z.k === 'bad' ? 'var(--bad)' : 'var(--warn)',
      'stroke-width':9, opacity:0.8}));
  });

  /* The reading, as an arc. On a centre-zero dial it grows out of the middle
     in whichever direction the value went, which is what makes a steering
     angle readable at a glance instead of requiring the number to be read. */
  var base = o.centre ? a0 + span / 2 : a0;
  var lead = sv('path', {fill:'none', stroke:'var(--acc)', 'stroke-width':9,
    'stroke-linecap':'round', d:''});
  g.appendChild(lead);

  var n = o.ticks || 5;
  for(var i = 0; i <= n; i++){
    var t = a0 + span * i / n;
    var p1 = pol(o.cx, o.cy, o.r - 7, t), p2 = pol(o.cx, o.cy, o.r - 16, t);
    g.appendChild(sv('line', {x1:p1[0].toFixed(1), y1:p1[1].toFixed(1),
      x2:p2[0].toFixed(1), y2:p2[1].toFixed(1),
      stroke:(o.centre && i * 2 === n) ? 'var(--txt)' : 'var(--dim)',
      'stroke-width':(o.centre && i * 2 === n) ? 2.4 : 1.6}));

    var lp = pol(o.cx, o.cy, o.r + 13, t);
    var tl = sv('text', {x:lp[0].toFixed(1), y:(lp[1] + 3.2).toFixed(1),
      fill:'var(--dim)', 'font-size':9, 'text-anchor':'middle'});
    tl.textContent = tickLabel(c.lo + (c.hi - c.lo) * i / n);
    g.appendChild(tl);

    if(i < n){
      for(var m = 1; m < 4; m++){
        var mt = t + (span / n) * (m / 4);
        var q1 = pol(o.cx, o.cy, o.r - 7, mt), q2 = pol(o.cx, o.cy, o.r - 11, mt);
        g.appendChild(sv('line', {x1:q1[0].toFixed(1), y1:q1[1].toFixed(1),
          x2:q2[0].toFixed(1), y2:q2[1].toFixed(1),
          stroke:'var(--line)', 'stroke-width':1.2}));
      }
    }
  }

  var needle = sv('g', {});
  needle.appendChild(sv('path', {fill:'var(--txt)',
    d:'M' + (o.cx - 3.4) + ' ' + o.cy + 'L' + o.cx + ' ' + (o.cy - (o.r - 13)) +
      'L' + (o.cx + 3.4) + ' ' + o.cy + 'Z'}));
  needle.appendChild(sv('circle', {cx:o.cx, cy:o.cy, r:5, fill:'var(--txt)'}));
  needle.appendChild(sv('circle', {cx:o.cx, cy:o.cy, r:2.2, fill:'var(--panel)'}));
  spin(needle, o.cx, o.cy);
  g.appendChild(needle);

  return {node:g, set:function(v, txt, z){
    var a = isFinite(v) ? ang(v) : a0;
    needle.style.transform = 'rotate(' + a.toFixed(2) + 'deg)';
    lead.setAttribute('stroke', zoneColour(z));
    if(!isFinite(v) || Math.abs(a - base) < 0.5){ lead.setAttribute('d', ''); }
    else { lead.setAttribute('d',
      arcp(o.cx, o.cy, o.r, Math.min(base, a), Math.max(base, a))); }
  }};
}

function compass(c){
  var cx = 74, cy = 74, r = 48;
  var g = sv('svg', {viewBox:'0 0 148 148', preserveAspectRatio:'xMidYMid meet'});
  g.appendChild(sv('circle', {cx:cx, cy:cy, r:r, fill:'none',
    stroke:'var(--track)', 'stroke-width':9}));

  var marks = ['0', '45', '90', '135', '180', '225', '270', '315'];
  for(var i = 0; i < 8; i++){
    var a = i * 45;
    var p1 = pol(cx, cy, r - 6, a), p2 = pol(cx, cy, r - (i % 2 ? 11 : 15), a);
    g.appendChild(sv('line', {x1:p1[0].toFixed(1), y1:p1[1].toFixed(1),
      x2:p2[0].toFixed(1), y2:p2[1].toFixed(1),
      stroke:i % 2 ? 'var(--line)' : 'var(--dim)', 'stroke-width':i % 2 ? 1.2 : 1.8}));
    if(i % 2 === 0){
      var lp = pol(cx, cy, r + 13, a);
      var t = sv('text', {x:lp[0].toFixed(1), y:(lp[1] + 3.2).toFixed(1),
        fill:'var(--dim)', 'font-size':9, 'text-anchor':'middle'});
      t.textContent = marks[i];
      g.appendChild(t);
    }
  }

  var needle = sv('g', {});
  needle.appendChild(sv('path', {fill:'var(--acc)',
    d:'M' + cx + ' ' + (cy - (r - 14)) + 'L' + (cx - 6) + ' ' + cy +
      'L' + (cx + 6) + ' ' + cy + 'Z'}));
  needle.appendChild(sv('path', {fill:'var(--dim)',
    d:'M' + cx + ' ' + (cy + (r - 22)) + 'L' + (cx - 5) + ' ' + cy +
      'L' + (cx + 5) + ' ' + cy + 'Z'}));
  needle.appendChild(sv('circle', {cx:cx, cy:cy, r:4.4, fill:'var(--txt)'}));
  spin(needle, cx, cy);
  g.appendChild(needle);

  return {node:g, set:function(v, txt, z){
    /* A compass reads all the way round, so the configured range maps onto a
       full turn rather than onto a sweep with a dead sector at the bottom. */
    var deg = isFinite(v) ? frac(c, v) * 360 : 0;
    needle.style.transform = 'rotate(' + deg.toFixed(2) + 'deg)';
    needle.firstChild.setAttribute('fill', zoneColour(z));
  }};
}

/* One builder for the horizontal bar and the vertical tank: same track, same
   fill, same bands, rotated. */
function bar(c, vert){
  var g, track, fill, L = 168;
  if(vert){
    g = sv('svg', {viewBox:'0 0 84 126', preserveAspectRatio:'xMidYMid meet'});
    L = 108;
    track = sv('rect', {x:26, y:8, width:32, height:L, rx:11, fill:'var(--track)'});
    fill  = sv('rect', {x:26, y:8 + L, width:32, height:0, rx:11, fill:'var(--acc)'});
  } else {
    g = sv('svg', {viewBox:'0 0 200 48', preserveAspectRatio:'xMidYMid meet'});
    track = sv('rect', {x:8, y:12, width:L + 16, height:17, rx:8.5, fill:'var(--track)'});
    fill  = sv('rect', {x:8, y:12, width:0, height:17, rx:8.5, fill:'var(--acc)'});
  }
  g.appendChild(track);

  zoneSpans(c).forEach(function(z){
    var f0 = frac(c, Math.min(z.a, z.b)), f1 = frac(c, Math.max(z.a, z.b));
    if(f1 - f0 < 0.01) return;
    var col = z.k === 'bad' ? 'var(--bad)' : 'var(--warn)';
    if(vert){
      g.appendChild(sv('rect', {x:26, y:(8 + L - f1 * L).toFixed(1), width:32,
        height:((f1 - f0) * L).toFixed(1), fill:col, opacity:0.22}));
    } else {
      g.appendChild(sv('rect', {x:(8 + 8 + f0 * L).toFixed(1), y:12,
        width:((f1 - f0) * L).toFixed(1), height:17, fill:col, opacity:0.22}));
    }
  });
  g.appendChild(fill);
  fill.setAttribute('style', 'transition:width var(--tt) linear,' +
                             'height var(--tt) linear,y var(--tt) linear');

  var lo = sv('text', {fill:'var(--dim)', 'font-size':9});
  var hi = sv('text', {fill:'var(--dim)', 'font-size':9});
  if(vert){
    lo.setAttribute('x', 64); lo.setAttribute('y', 8 + L);
    hi.setAttribute('x', 64); hi.setAttribute('y', 14);
  } else {
    lo.setAttribute('x', 8);   lo.setAttribute('y', 43);
    hi.setAttribute('x', 192); hi.setAttribute('y', 43);
    hi.setAttribute('text-anchor', 'end');
  }
  lo.textContent = tickLabel(c.lo);
  hi.textContent = tickLabel(c.hi);
  g.appendChild(lo); g.appendChild(hi);

  return {node:g, set:function(v, txt, z){
    var f = isFinite(v) ? frac(c, v) : 0;
    if(vert){
      fill.setAttribute('height', (f * L).toFixed(1));
      fill.setAttribute('y', (8 + L - f * L).toFixed(1));
    } else {
      fill.setAttribute('width', (f * (L + 16)).toFixed(1));
    }
    fill.setAttribute('fill', zoneColour(z));
  }};
}

function thermo(c){
  var g = sv('svg', {viewBox:'0 0 124 132', preserveAspectRatio:'xMidYMid meet'});
  var top = 10, H = 74, x = 44, w = 22, bx = x + w / 2, by = top + H + 24, br = 19;

  g.appendChild(sv('rect', {x:x, y:top, width:w, height:H + 16, rx:w / 2,
    fill:'var(--track)'}));
  g.appendChild(sv('circle', {cx:bx, cy:by, r:br, fill:'var(--track)'}));

  /* Beside the tube, not over it: a band painted across the glass reads as
     something inside the thermometer rather than as a mark on the scale. */
  zoneSpans(c).forEach(function(z){
    var f0 = frac(c, Math.min(z.a, z.b)), f1 = frac(c, Math.max(z.a, z.b));
    if(f1 - f0 < 0.01) return;
    g.appendChild(sv('rect', {x:x + w + 3, y:(top + H - f1 * H).toFixed(1),
      width:5, height:((f1 - f0) * H).toFixed(1), rx:2.5,
      fill:z.k === 'bad' ? 'var(--bad)' : 'var(--warn)', opacity:0.75}));
  });

  var col  = sv('rect', {x:x + 5, y:top + H, width:w - 10, height:0,
    fill:'var(--acc)'});
  var bulb = sv('circle', {cx:bx, cy:by, r:br - 5, fill:'var(--acc)'});
  col.setAttribute('style', 'transition:height var(--tt) linear,y var(--tt) linear');
  g.appendChild(col);
  g.appendChild(bulb);

  for(var i = 0; i <= 4; i++){
    var y = top + H - (H * i / 4);
    g.appendChild(sv('line', {x1:x - 9, y1:y, x2:x - 3, y2:y,
      stroke:'var(--line)', 'stroke-width':1.5}));
    var t = sv('text', {x:x - 12, y:y + 3.2, fill:'var(--dim)', 'font-size':9,
      'text-anchor':'end'});
    t.textContent = tickLabel(c.lo + (c.hi - c.lo) * i / 4);
    g.appendChild(t);
  }

  return {node:g, set:function(v, txt, z){
    var f = isFinite(v) ? frac(c, v) : 0;
    col.setAttribute('height', (f * H).toFixed(1));
    col.setAttribute('y', (top + H - f * H).toFixed(1));
    col.setAttribute('fill', zoneColour(z));
    bulb.setAttribute('fill', zoneColour(z));
  }};
}

/* A number and where it has been. The history lives in the browser: the logger
   sends one value per poll and never has to remember anything. */
function spark(c){
  var W = 196, H = 54, N = 90;
  var g = sv('svg', {viewBox:'0 0 200 60', preserveAspectRatio:'none'});
  var line = sv('polyline', {fill:'none', stroke:'var(--acc)', 'stroke-width':2,
    'stroke-linejoin':'round', 'stroke-linecap':'round', points:''});
  var area = sv('polygon', {fill:'var(--acc)', opacity:0.13, points:''});
  g.appendChild(sv('line', {x1:2, y1:H/2 + 3, x2:W, y2:H/2 + 3,
    stroke:'var(--line)', 'stroke-width':1, 'stroke-dasharray':'3 4'}));
  g.appendChild(area); g.appendChild(line);
  var dot = sv('circle', {cx:-9, cy:0, r:3, fill:'var(--acc)'});
  g.appendChild(dot);

  var hist = [];
  return {node:g, set:function(v, txt, z){
    if(isFinite(v)) hist.push(v);
    while(hist.length > N) hist.shift();
    if(hist.length < 2){ line.setAttribute('points', ''); return; }

    /* Newest at the right, growing leftwards, so the trace is anchored where
       the eye looks for "now" and a half-full history is a short line at the
       right rather than a stub in the corner. */
    var pts = '', i, x, y, first = W;
    for(i = 0; i < hist.length; i++){
      x = W - (hist.length - 1 - i) * ((W - 4) / (N - 1));
      y = 3 + (H - 6) * (1 - frac(c, hist[i]));
      if(i === 0) first = x;
      pts += x.toFixed(1) + ',' + y.toFixed(1) + ' ';
    }
    line.setAttribute('points', pts);
    area.setAttribute('points', first.toFixed(1) + ',' + (H + 3) + ' ' + pts +
                                x.toFixed(1) + ',' + (H + 3));
    line.setAttribute('stroke', zoneColour(z));
    area.setAttribute('fill', zoneColour(z));
    dot.setAttribute('cx', x.toFixed(1));
    dot.setAttribute('cy', y.toFixed(1));
    dot.setAttribute('fill', zoneColour(z));
  }};
}

/* A named state rather than a number: a gear, a mode, a fault line. The colour
   is guessed from the word, because the DBC says what a value is called and
   never says whether it is good news. */
var GOOD = /^(off|ok|normal|none|ready|idle|forward|closed|inactive|no|0|false)$/i;
var BUSY = /^(on|run|running|active|open|enabled|yes|1|true|auto|drive)$/i;
var BAD  = /(fault|error|fail|alarm|emerg|overheat|overload|critical|stop|trip|bad)/i;
var WARNW = /(warn|caution|degrad|limit|reduced|service)/i;
function pill(c){
  var p = el('div', 'pill', '--');
  return {node:p, set:function(v, txt, z){
    var s = (txt === null || txt === undefined || txt === '') ? '--' : String(txt);
    p.textContent = s;
    var k = 'p-acc';
    if(z === 'bad' || BAD.test(s)) k = 'p-bad';
    else if(z === 'warn' || WARNW.test(s)) k = 'p-warn';
    else if(GOOD.test(s)) k = 'p-ok';
    else if(BUSY.test(s)) k = 'p-acc';
    p.className = 'pill ' + k;
  }};
}

var WIDGETS = [
  {id:'gauge',   name:'Gauge',   hint:'speed, rpm, flow'},
  {id:'arc',     name:'Half',    hint:'pressure, load'},
  {id:'angle',   name:'Angle',   hint:'steering, tilt'},
  {id:'compass', name:'Compass', hint:'heading, yaw'},
  {id:'bar',     name:'Bar',     hint:'percentages'},
  {id:'level',   name:'Tank',    hint:'fuel, oil level'},
  {id:'thermo',  name:'Temp',    hint:'temperatures'},
  {id:'number',  name:'Number',  hint:'anything'},
  {id:'spark',   name:'Trend',   hint:'drift over time'},
  {id:'state',   name:'State',   hint:'gear, mode, faults'}
];

function makeWidget(c){
  switch(c.w){
    case 'gauge':   return dial(c, {vb:'0 0 200 132', cx:100, cy:86, r:60, a0:-120, a1:120, ticks:5});
    case 'arc':     return dial(c, {vb:'0 0 200 100', cx:100, cy:86, r:60, a0:-90, a1:90, ticks:4});
    case 'angle':   return dial(c, {vb:'0 0 200 104', cx:100, cy:88, r:60, a0:-80, a1:80, ticks:4, centre:true});
    case 'compass': return compass(c);
    case 'bar':     return bar(c, false);
    case 'level':   return bar(c, true);
    case 'thermo':  return thermo(c);
    case 'spark':   return spark(c);
    case 'state':   return pill(c);
    default:        return {node:null, set:function(){}};
  }
}

function sigName(ref){
  var dot = String(ref || '').lastIndexOf('.');
  return dot < 0 ? String(ref || '') : ref.slice(dot + 1);
}
function cellTitle(c){ return c.label || sigName(c.sig); }
</script>
)HTML";

/* ==========================================================================
 *  PART 4 - the application: tabs, the grid, customization, diagnostics.
 * ======================================================================== */
static const char PAGE_4[] PROGMEM = R"HTML(<script>
"use strict";
var TAB = 'dash', EDIT = false, cellObjs = {}, timers = [], fails = 0, seq = 0;
var LIVE = {};            /* Message.Signal -> latest text, for the editors  */
var lastTicket = 0, ARMED = false, ARMLEFT = 0, CYC = 0, CANTX = 1;

function postForm(url, obj){
  var b = Object.keys(obj).map(function(k){
    return encodeURIComponent(k) + '=' + encodeURIComponent(obj[k]);
  }).join('&');
  return fetch(url, {method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:b});
}
function toast(title, msg, kind){
  var t = el('div', 'toast' + (kind ? ' ' + kind : ''));
  t.appendChild(el('b', null, title));
  t.appendChild(document.createTextNode(msg));
  q('toasts').appendChild(t);
  setTimeout(function(){
    t.style.opacity = 0;
    t.style.transition = 'opacity .3s';
    setTimeout(function(){ if(t.parentNode) t.remove(); }, 320);
  }, 4200);
}
function setDot(id, cls){ q(id).className = 'dot ' + cls; }

/* ---- tabs -------------------------------------------------------------- *
 * Only the visible tab is polled, and a backgrounded browser polls nothing.
 * The Dashboard needs exactly one request per update because /api/dash now
 * carries the health counters as well as the cell values - the two big tables
 * that make /api/status expensive are only on the Bus tab, which is the only
 * tab that shows them.                                                      */
var TABS = ['dash','bus','send','log'];
function showTab(name){
  if(TABS.indexOf(name) < 0) name = 'dash';
  TAB = name;
  /* In the address bar, so a tab can be bookmarked - "open the logger on the
     log" is a reasonable thing to want on a machine you visit often. */
  if(location.hash.slice(1) !== name) history.replaceState(null, '', '#' + name);
  TABS.forEach(function(n){ q('v_' + n).hidden = (n !== name); });
  Array.prototype.forEach.call(q('tabs').children, function(b){
    b.classList.toggle('on', b.dataset.tab === name);
  });
  /* The Send tab needs the frame map: a value whose input is "pick from the
     frame map's own names" cannot draw its list without it, and would silently
     fall back to a number box - which is the one input this feature exists to
     avoid. */
  if(name === 'send') loadDbc().then(renderSend);
  startPolls();
}
function stopPolls(){ timers.forEach(clearInterval); timers = []; }
function startPolls(){
  stopPolls();
  if(document.hidden) return;

  if(TAB === 'dash'){
    /* While the grid is being customized nothing on it is showing a live value
       - the cells are placeholders being dragged around - so the fast poll is
       pointless work for the logger. Drop to a heartbeat that only keeps the
       health cards and the connection indicator honest. */
    pollDash();
    timers.push(setInterval(pollDash, EDIT ? 2000 : CFG.poll));
  } else if(TAB === 'bus'){
    pollStatus();
    timers.push(setInterval(pollStatus, 600));
  } else if(TAB === 'send'){
    pollDash();
    timers.push(setInterval(pollDash, 700));
  } else {
    pollLog(); pollDash();
    timers.push(setInterval(pollLog, 700));
    timers.push(setInterval(pollDash, 3000));
  }
}
document.addEventListener('visibilitychange', startPolls);

/* ---- building the grid ------------------------------------------------- */
function buildCell(slot){
  var c = CFG.cells[slot];
  var e = el('div', 'cell slot');
  e.dataset.slot = slot;

  if(!c){
    e.className = 'cell slot empty';
    e.appendChild(el('div', null, '+'));
    e.appendChild(el('div', null, 'Add a value'));
    return {el:e, empty:true};
  }

  var tools = el('div', 'tools');
  var bEdit = el('button', null, '✎'); bEdit.title = 'Edit this cell';
  var bDel  = el('button', null, '×'); bDel.title = 'Remove this cell';
  bEdit.dataset.act = 'edit'; bDel.dataset.act = 'del';
  tools.appendChild(bEdit); tools.appendChild(bDel);
  e.appendChild(tools);

  e.appendChild(el('div', 'cname', cellTitle(c)));

  var w = makeWidget(c);
  var gfx = el('div', 'gfx');
  if(w.node) gfx.appendChild(w.node);
  e.appendChild(gfx);

  var val = el('div', 'cval' + (w.node && c.w !== 'state' ? '' : ' big'),
               c.w === 'state' ? '' : '--');
  if(c.w === 'state') val.style.display = 'none';
  e.appendChild(val);
  e.appendChild(el('div', 'cunit', c.w === 'state' ? '' : (c.unit || '')));

  return {el:e, w:w, val:val, unit:e.lastChild, cfg:c};
}

function renderGrid(){
  var g = q('grid');
  g.innerHTML = '';
  cellObjs = {};
  g.style.setProperty('--cols', CFG.cols);
  document.documentElement.style.setProperty('--tt',
    Math.max(120, Math.round(CFG.poll * 0.9)) + 'ms');

  var total = cellsCount(), used = [], i;
  for(i = 0; i < total; i++) if(CFG.cells[i]) used.push(i);

  /* In view mode a trailing row of empty slots is dead space, so the grid
     stops after the row holding the last cell. In customize mode every slot is
     there, because an empty one is where the next cell goes. */
  var show = total;
  if(!EDIT) show = used.length
    ? (Math.floor(used[used.length - 1] / CFG.cols) + 1) * CFG.cols : 0;

  for(i = 0; i < show; i++){
    var o = buildCell(i);
    if(o.empty && !EDIT) o.el.style.visibility = 'hidden';
    cellObjs[i] = o;
    g.appendChild(o.el);
  }

  q('emptyhint').hidden = (used.length > 0) || EDIT;
  q('colnum').textContent = CFG.cols;
  q('rownum').textContent = CFG.rows;
  q('dashnote').textContent = used.length
    ? used.length + (used.length === 1 ? ' value' : ' values') +
      ' · updating ' + (1000 / CFG.poll).toFixed(1) + ' times a second'
    : '';

  /* Rebuilding the grid leaves every cell showing "--" until the next poll,
     and in customize mode the next poll is up to two seconds away. Ask once,
     now: a structural change happens when somebody drops a cell or fills the
     grid, not on every frame, so this is one extra request per action. */
  if(EDIT && used.length) pollDash();
}

/* One poll's values. Touches nothing structural: a needle moves and one string
   changes, which is the whole update. */
function applyValues(v, f){
  Object.keys(cellObjs).forEach(function(k){
    var o = cellObjs[k], i = +k;
    if(!o || o.empty) return;

    var raw = (v && i < v.length) ? v[i] : null;
    var fresh = !!(f && f[i]);
    var c = o.cfg;

    if(raw === false){
      /* Configured, but this frame map has no such signal - which is what
         swapping SD cards looks like. Say so on the cell, rather than a dash
         that could equally mean "not arrived yet". */
      o.el.classList.add('miss');
      o.el.classList.remove('stale');
      o.val.textContent = 'unknown';
      o.val.className = 'cval big v-warn';
      o.val.style.display = '';
      if(o.unit) o.unit.textContent = 'not in this frame map';
      return;
    }
    o.el.classList.remove('miss');
    if(o.unit && c.w !== 'state') o.unit.textContent = c.unit || '';

    var num = (raw === null || raw === '') ? NaN : parseFloat(raw);
    var z = zoneOf(c, num);

    o.el.classList.toggle('stale', !fresh);
    if(c.w !== 'state'){
      o.val.textContent = isFinite(num) ? fmtVal(num, decOf(c)) : (raw ? raw : '--');
      o.val.className = 'cval' + (o.w.node ? '' : ' big') + ' v-' + z;
    }
    if(o.w.set) o.w.set(num, raw, z);
    if(c.sig) LIVE[c.sig] = raw;
  });
}

/* ---- the health tiles, driven from either endpoint --------------------- *
 * /api/dash and /api/status carry the same counters under the same names, so
 * whichever tab is open paints these from whatever it was already fetching.  */
function paintHealth(d){
  q('conn').textContent = (d.ap ? 'Hotspot ' : 'Wi-Fi ') + d.ip;

  if(!d.sd){ setDot('d_sd','bad'); q('t_sd').textContent = 'NOT FOUND';
             q('s_sd').textContent = 'Insert a FAT32 card and restart'; }
  else if(d.sdErr){ setDot('d_sd','bad'); q('t_sd').textContent = 'WRITE ERROR';
             q('s_sd').textContent = 'Card may be full or was removed'; }
  else { setDot('d_sd','ok'); q('t_sd').textContent = 'READY';
             q('s_sd').textContent = d.sdType + ', ' + (d.sdMB/1024).toFixed(1) + ' GB'; }

  if(!d.can){
    setDot('d_irq','warn'); q('t_irq').textContent = 'IDLE';
    q('s_irq').textContent = 'No traffic, nothing to interrupt on';
  } else if(d.intStuck){
    setDot('d_irq','bad'); q('t_irq').textContent = 'NOT FIRING';
    q('s_irq').textContent = 'Running on the fallback poll - check the INT wire';
  } else {
    setDot('d_irq','ok'); q('t_irq').textContent = d.irq.toLocaleString() + ' /s';
    q('s_irq').textContent = 'ISR healthy - INT line ' +
                             (d.intLevel ? 'idle high' : 'asserted');
  }

  var L = d.load;
  q('t_load').textContent = L + '%';
  q('s_load').textContent = d.fps.toLocaleString() + ' frames/s';
  var bar2 = q('loadbar');
  bar2.style.width = Math.min(L, 100) + '%';
  if(L < 60){ setDot('d_load','ok');   bar2.style.background = 'var(--ok)'; }
  else if(L < 80){ setDot('d_load','warn'); bar2.style.background = 'var(--warn)'; }
  else { setDot('d_load','bad'); bar2.style.background = 'var(--bad)'; }

  if(d.can){ setDot('d_can','ok'); q('t_can').textContent = 'RECEIVING';
             q('s_can').textContent = d.fps + ' frames/s'; }
  else { setDot('d_can','bad'); q('t_can').textContent = 'NO DATA';
             q('s_can').textContent = 'Check the wiring, bit rate and crystal'; }

  if(!d.lost){ setDot('d_lost','ok'); q('t_lost').textContent = 'ALL GOOD';
               q('s_lost').textContent = 'No frames lost - up to ' + d.risk +
                 ' ms at risk if power is cut'; }
  else {
    /* The controller's overflow flags are sticky: they say a frame was lost,
       never how many. So this is a FLOOR, and it says so - a number presented
       as exact when it is not is worse than no number. */
    setDot('d_lost','bad');
    q('t_lost').textContent = '\u2265 ' + d.lost + ' LOST';
    var parts = [];
    if(d.qDrop) parts.push(d.qDrop + ' the writer could not keep up with');
    if(d.ovfEv) parts.push(d.ovfEv + ' controller overflow(s), each costing at '
                           + 'least one frame and usually more');
    q('s_lost').textContent = parts.length ? parts.join(' \u00b7 ')
                                           : 'Some frames could not be saved';
  }

  if(d.rec){
    setDot('d_rec','ok rec');
    q('t_rec').textContent = 'REC  ' + hms(d.elapsed);
    q('s_rec').textContent = d.file + '  -  ' + d.rows.toLocaleString() + ' rows, ' +
                             (d.kb/1024).toFixed(1) + ' MB';
    q('btn').textContent = 'STOP'; q('btn').className = 'stop';
  } else if(d.pf){
    setDot('d_rec','bad');
    q('t_rec').textContent = 'POWER LOSS';
    q('s_rec').textContent = d.file + ' was closed safely - press START for a new file';
    q('btn').textContent = 'START'; q('btn').className = 'start';
  } else {
    setDot('d_rec','warn');
    q('t_rec').textContent = 'STOPPED';
    q('s_rec').textContent = 'Nothing is being saved';
    q('btn').textContent = 'START'; q('btn').className = 'start';
  }

  q('s_up').textContent = 'up ' + hms(Math.floor(d.up/1000)) + ', ' +
                          Math.round(d.heap/1024) + ' KB free';
  q('foot').textContent = d.fw;
}

/* ---- polling ----------------------------------------------------------- */
function pollDash(){
  return fetch('/api/dash').then(function(r){ return r.json(); }).then(function(d){
    fails = 0;
    if(GEN !== 0 && d.gen !== GEN && !EDIT){
      GEN = d.gen;
      loadCfg();            /* somebody saved from another browser */
      return;
    }
    GEN = d.gen;
    if(d.max) MAXCELLS = d.max;
    if(d.poll && d.poll !== CFG.poll && !EDIT){ CFG.poll = d.poll; startPolls(); }

    paintHealth(d);
    applyValues(d.v, d.f);

    ARMED = !!d.arm;
    ARMLEFT = d.armLeft || 0;
    CYC = d.cyc || 0;
    CANTX = d.canTx;
    paintArm();
    if(TAB === 'send') paintSendState();

    (d.tx || []).forEach(function(o){
      if(o.t <= lastTicket) return;
      lastTicket = o.t;
      var ok = (o.s === 0);
      toast(ok ? 'Sent' : 'Not sent',
            o.id + ' — ' + o.m +
            (o.c ? ' (the value was clamped to what the signal can hold)' : '') +
            (!ok && o.tec ? ' [TEC ' + (o.tec > 0 ? '+' : '') + o.tec + ']' : ''),
            ok ? 'ok' : 'bad');
    });
  }).catch(function(){
    if(++fails > 2) q('conn').textContent = 'connection lost';
  });
}

function paintIds(list){
  var body = q('ids'), html = '', i;
  for(i = 0; i < list.length; i++){
    var e2 = list[i];
    html += '<tr class="' + (e2.k ? '' : 'un') + '"><td class="mono">' + esc(e2.id) +
            '</td><td class="mono">' + esc(e2.d) + '</td><td>' +
            e2.n.toLocaleString() + '</td><td>' + e2.r + '/s</td><td>' +
            (e2.k ? 'yes' : 'raw') + '</td></tr>';
  }
  body.innerHTML = html || '<tr><td colspan="5">nothing received yet</td></tr>';
}

function paintSigs(list, mapped){
  var body = q('sigs'), html = '', i;
  for(i = 0; i < list.length; i++){
    var e2 = list[i];
    LIVE[e2.m + '.' + e2.s] = e2.v;
    html += '<tr><td class="c1 ell" title="' + esc(e2.m) + '">' + esc(e2.m) +
            '</td><td class="c2 ell" title="' + esc(e2.s) + '">' + esc(e2.s) +
            '</td><td class="c3 num mono">' + esc(e2.v) +
            '</td><td class="c4">' + esc(e2.u) + '</td></tr>';
  }
  if(!html){
    html = '<tr><td colspan="4">' + (mapped
      ? 'no mapped frame has arrived yet'
      : 'no frame map on the card - see the raw frames below') + '</td></tr>';
  }
  body.innerHTML = html;
}

function pollStatus(){
  return fetch('/api/status').then(function(r){ return r.json(); }).then(function(d){
    fails = 0;
    paintHealth(d);
    paintSigs(d.sig, d.dbc);
    q('s_sigs').textContent = d.dbc
      ? ('frame map loaded: ' + d.dbcMsg + ' messages, ' + d.dbcSig + ' signals' +
         (d.sigMore ? '  -  showing the first ' + d.sig.length : ''))
      : 'add a DBC file to the card to decode signals in real time';
    paintIds(d.ids);
    q('s_ids').textContent = d.idMore
      ? 'more identifiers are on the bus than the table tracks - all of them ' +
        'are still recorded'
      : 'every identifier seen since the recording started';
  }).catch(function(){ if(++fails > 2) q('conn').textContent = 'connection lost'; });
}

function pollLog(){
  return fetch('/api/log?since=' + seq).then(function(r){ return r.json(); })
    .then(function(d){
      seq = d.seq;
      if(!d.lines.length) return;
      var t = q('term');
      var atBottom = t.scrollHeight - t.scrollTop - t.clientHeight < 40;
      for(var i = 0; i < d.lines.length; i++){
        var s = d.lines[i], div = el('div');
        var m = s.match(/^\[[^\]]*\]\s(\w)\s/);
        div.className = m ? m[1] : '';
        div.textContent = s;
        t.appendChild(div);
      }
      while(t.childNodes.length > 300) t.removeChild(t.firstChild);
      if(atBottom) t.scrollTop = t.scrollHeight;
    }).catch(function(){});
}

/* ---- saving ------------------------------------------------------------ */
var saveTimer = null;
function markDirty(){
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveCfg, 1200);
}
function saveCfg(){
  clearTimeout(saveTimer);
  return fetch('/api/dash/cfg', {method:'POST', body:dumpCfg()})
    .then(function(r){ return r.json(); })
    .then(function(d){
      GEN = d.gen;
      if(!d.ok) toast('Not saved', 'The logger could not store the layout', 'bad');
      else if(d.missing) toast('Saved',
        d.missing + ' cell(s) name a signal this frame map does not have', 'bad');
    })
    .catch(function(){ toast('Not saved', 'The logger did not answer', 'bad'); });
}
function loadCfg(){
  return fetch('/api/dash/cfg').then(function(r){ return r.text(); })
    .then(function(t){
      CFG = parseCfg(t);
      renderRoleBtn();
      renderGrid();
      renderSend();
      startPolls();
    });
}
/* Every cell and setpoint the loaded frame map cannot account for, removed and
   the gaps closed. Returns how many went.

   Deliberately NOT what happens when the page merely reloads: a cell that
   cannot resolve because the logger has no DBC on its card is a temporary
   problem, and throwing away a layout over it would be much worse than drawing
   it as unresolvable. This runs only when somebody has actually loaded a
   different map. */
function dropUnresolved(){
  var have = {};
  DBC.m.forEach(function(m){
    m.s.forEach(function(sg){ have[m.n + '.' + sg.n] = 1; });
  });

  var gone = 0;
  function prune(map, isRaw){
    var keep = {}, w = 0;
    Object.keys(map).map(Number).sort(function(a, b){ return a - b; })
      .forEach(function(k){
        var e = map[k];
        /* A one-off frame names an identifier, not a signal, so no frame map
           can invalidate it. */
        if(isRaw && e && !e.sig){ keep[w++] = e; return; }
        if(e && e.sig && have[e.sig]){ keep[w++] = e; return; }
        gone++;
      });
    return keep;
  }
  CFG.cells = prune(CFG.cells, false);
  CFG.tx    = prune(CFG.tx,    true);
  return gone;
}

function loadDbc(force){
  if(DBC.m.length && !force) return Promise.resolve();
  return fetch('/api/signals').then(function(r){ return r.json(); })
    .then(function(d){ DBC = d; });
}

/* ---- customize mode ---------------------------------------------------- */
function setEdit(on){
  EDIT = on;
  document.body.classList.toggle('edit', on);
  q('editbar').hidden = !on;
  q('viewbar').hidden = on;
  if(on) loadDbc();
  renderGrid();
  startPolls();
  if(!on) saveCfg();
}
function step(which, by){
  var lim = which === 'cols' ? 6 : 8;
  var v = Math.max(1, Math.min(lim, CFG[which] + by));
  if(v === CFG[which]) return;
  CFG[which] = v;
  renderGrid();
  markDirty();
}

/* Drag and drop on pointer events rather than the HTML5 drag API, which does
   not fire on touch at all - and this page is used on a phone at least as often
   as on a laptop. */
var drag = null;
function ghostTo(e){
  if(!drag) return;
  drag.ghost.style.left = e.clientX + 'px';
  drag.ghost.style.top  = e.clientY + 'px';
}
q('grid').addEventListener('pointerdown', function(e){
  if(!EDIT || e.button > 0) return;
  var cell = e.target.closest ? e.target.closest('.cell.slot') : null;
  if(!cell) return;

  var act = e.target.closest('[data-act]');
  if(act){
    var s = +cell.dataset.slot;
    if(act.dataset.act === 'edit') openEditor(s);
    else { removeCell(s); renderGrid(); markDirty(); }
    return;
  }

  var slot = +cell.dataset.slot;
  if(!CFG.cells[slot]){ openEditor(slot); return; }

  e.preventDefault();
  var r = cell.getBoundingClientRect();
  var ghost = cell.cloneNode(true);
  ghost.id = 'ghost';
  ghost.className = 'cell';
  ghost.style.width = r.width + 'px';
  ghost.style.height = r.height + 'px';
  document.body.appendChild(ghost);
  cell.classList.add('lift');

  drag = {from:slot, cell:cell, ghost:ghost, over:null, moved:false};
  ghostTo(e);
  try { cell.setPointerCapture(e.pointerId); } catch(err){}
});
document.addEventListener('pointermove', function(e){
  if(!drag) return;
  drag.moved = true;
  ghostTo(e);
  var t = document.elementFromPoint(e.clientX, e.clientY);
  var over = (t && t.closest) ? t.closest('.cell.slot') : null;
  if(drag.over && drag.over !== over) drag.over.classList.remove('over');
  if(over && over !== drag.cell){ over.classList.add('over'); drag.over = over; }
  else drag.over = null;
});
document.addEventListener('pointerup', function(e){
  if(!drag) return;
  var dg = drag; drag = null;
  dg.ghost.remove();
  dg.cell.classList.remove('lift');

  if(dg.over){
    dg.over.classList.remove('over');
    var to = +dg.over.dataset.slot;
    /* A swap, not an insert: whatever was there moves to where this came from,
       so nothing is ever silently overwritten. */
    var a = CFG.cells[dg.from], b = CFG.cells[to];
    if(b) CFG.cells[dg.from] = b; else delete CFG.cells[dg.from];
    CFG.cells[to] = a;
    renderGrid();
    markDirty();
  } else if(!dg.moved){
    openEditor(dg.from);
  }
});

/* Lay the bus out automatically - the fastest route from a fresh logger to a
   dashboard worth looking at, and what makes the whole feature discoverable. */
/* Two ways to fill the grid, because there are two situations.

   'map'  - at a desk, before going out, with a frame map loaded and no bus
            anywhere. Lays out what the file DESCRIBES, in file order, as far
            as the grid goes. Nothing has to be connected for this to work.
   'bus'  - standing at the machine with traffic running. Same list, but the
            signals actually arriving are placed first, and the ones that are
            only in the file are capped, because a grid full of cells that will
            never update is worse than a small one where everything moves.

   Both end at the same place: a starting point you then drag around. */
function fillCells(source){
  var jobs = [loadDbc()];
  if(source === 'bus'){
    jobs.push(fetch('/api/status').then(function(r){ return r.json(); }));
  }
  return Promise.all(jobs).then(function(res){
    var live = {};
    if(source === 'bus'){
      (res[1].sig || []).forEach(function(s){ live[s.m + '.' + s.s] = 1; });
    }

    if(!DBC.m.length){
      toast('No frame map', 'Put a .dbc on the card as /frames.dbc, or add '
            + 'cells one at a time', 'bad');
      return;
    }

    var taken = {};
    Object.keys(CFG.cells).forEach(function(k){ taken[CFG.cells[k].sig] = 1; });

    var pick = [], skipped = 0;
    DBC.m.forEach(function(m){
      /* A message this logger is the one transmitting is a command, not a
         reading: its "value" is whatever we last wrote. */
      if(CFG.role && m.tx === CFG.role){ skipped++; return; }
      m.s.forEach(function(s){
        var ref = m.n + '.' + s.n;
        if(!taken[ref]) pick.push({ref:ref, s:s, live:live[ref] ? 1 : 0});
      });
    });
    if(source === 'bus'){
      /* stable: equal keys keep frame-map order, so the result is repeatable */
      pick.sort(function(a, b){ return b.live - a.live; });
    }

    var room  = Math.min(MAXCELLS, CFG.cols * 8);
    var slot = 0, added = 0, seen = 0;
    pick.forEach(function(p){
      while(CFG.cells[slot]) slot++;
      if(slot >= room) return;
      if(source === 'bus' && p.live) seen++;
      if(source === 'bus' && added >= 12 && !p.live) return;
      CFG.cells[slot] = cellFromSignal(p.ref, p.s);
      added++;
    });

    var keys = Object.keys(CFG.cells).map(Number);
    if(keys.length){
      var need = Math.ceil((Math.max.apply(null, keys) + 1) / CFG.cols);
      if(need > CFG.rows) CFG.rows = Math.min(8, need);
    }
    renderGrid();
    markDirty();

    if(!added){
      toast('Nothing to add', 'Every signal in the frame map is already on the '
            + 'dashboard', 'ok');
    } else if(source === 'bus'){
      toast('Filled from the bus', added + ' cell(s) added, ' + seen
            + ' of them arriving right now — drag them around, or tap one to '
            + 'change how it is drawn', 'ok');
    } else {
      toast('Filled from the frame map', added + ' cell(s) added'
            + (skipped ? ', ' + skipped + ' message(s) ' + CFG.role
                       + ' sends left for the Send tab' : '')
            + ' — drag them around, or tap one to change how it is drawn', 'ok');
    }
  });
}

/* What a signal should look like, before anyone has said otherwise. */
function guessWidget(ref, s){
  var n = sigName(ref).toLowerCase(), u = (s.u || '').toLowerCase();
  if(s.v && s.v.length) return 'state';
  if(s.b === 1) return 'state';
  if(/temp|coolant|therm/.test(n) || /^(degc|degf|c|f|k|°c|°f)$/.test(u)) return 'thermo';
  if(/level|tank|fuel|soc|charge|volume/.test(n)) return 'level';
  if(/head|yaw|course|bearing|azimuth|crank/.test(n)) return 'compass';
  if(/angle|steer|tilt|roll|pitch|articul|slope/.test(n)) return 'angle';
  if(/press/.test(n) || /^(bar|psi|kpa|mpa|pa)$/.test(u)) return 'arc';
  if(/speed|rpm|flow|revs|velocity/.test(n) ||
     /^(km\/h|kph|mph|m\/s|rpm|l\/min|l\/h|min-1)$/.test(u)) return 'gauge';
  if(u === '%' || u === 'pct') return 'bar';
  if(/^(deg|°|rad)$/.test(u)) return (s.lo < 0 && s.r) ? 'angle' : 'compass';
  return 'number';
}
/* SteeringAngle -> "Steering angle". The title has to be WRITTEN DOWN rather
   than left empty and derived at draw time: an empty label meant reopening a
   cell showed a blank Title box under a cell that plainly had a title. */
function prettyName(ref){
  var n = sigName(ref).replace(/[_.]+/g, ' ')
                      .replace(/([a-z0-9])([A-Z])/g, '$1 $2')
                      .replace(/\s+/g, ' ').trim();
  return n ? n.charAt(0).toUpperCase() + n.slice(1).toLowerCase() : n;
}
function cellFromSignal(ref, s){
  var lo = s.r ? s.lo : s.blo, hi = s.r ? s.hi : s.bhi;
  /* A 32-bit counter's true limits are in the billions, which draws a gauge
     that never moves. Anything that wide is shown as a number instead. */
  if(!isFinite(lo) || !isFinite(hi) || hi - lo > 1e7){ lo = 0; hi = 100; }
  return {
    w: guessWidget(ref, s), sig: ref, label: prettyName(ref), unit: s.u || '',
    lo: lo, hi: hi, dec: isFinite(s.d) ? Math.min(s.d, 3) : null,
    warn: null, crit: null, lowbad: 0
  };
}
</script>
)HTML";

/* ==========================================================================
 *  PART 5 - the cell editor, the transmit view, and start-up.
 * ======================================================================== */
static const char PAGE_5[] PROGMEM = R"HTML(<script>
"use strict";
var edSlot = -1, edCfg = null, edPicked = false;

/* Small glyphs for the style picker. Drawn separately from the real widgets
   rather than rendered small: a gauge at 60 by 38 pixels is a smudge with
   unreadable tick labels, and the point of a picker is recognisability. */
function miniIcon(id){
  var g = sv('svg', {viewBox:'0 0 60 38'});
  function p(path, o){
    var a = {d:path, fill:'none', stroke:'currentColor', 'stroke-width':3,
             'stroke-linecap':'round'}, k;
    if(o) for(k in o) a[k] = o[k];
    g.appendChild(sv('path', a));
  }
  function r(x, y, w, h, o){
    var a = {x:x, y:y, width:w, height:h, rx:Math.min(w,h)/2,
             fill:'currentColor'}, k;
    if(o) for(k in o) a[k] = o[k];
    g.appendChild(sv('rect', a));
  }
  switch(id){
    case 'gauge':
      p('M12 30 A18 18 0 1 1 48 30', {opacity:.35});
      p('M12 30 A18 18 0 0 1 22 14', {stroke:'var(--acc)'});
      p('M30 30 L38 18', {'stroke-width':2.4}); break;
    case 'arc':
      p('M10 28 A20 20 0 0 1 50 28', {opacity:.35});
      p('M10 28 A20 20 0 0 1 26 9', {stroke:'var(--acc)'});
      p('M30 28 L30 12', {'stroke-width':2.4}); break;
    case 'angle':
      p('M10 28 A20 20 0 0 1 50 28', {opacity:.35});
      p('M30 8 A20 20 0 0 1 46 17', {stroke:'var(--acc)'});
      p('M30 28 L41 14', {'stroke-width':2.4});
      p('M30 8 L30 13', {'stroke-width':2, opacity:.8}); break;
    case 'compass':
      g.appendChild(sv('circle', {cx:30, cy:19, r:15, fill:'none',
        stroke:'currentColor', 'stroke-width':3, opacity:.35}));
      p('M30 19 L38 8', {stroke:'var(--acc)'});
      p('M30 19 L25 27', {'stroke-width':2.4, opacity:.6}); break;
    case 'bar':
      r(8, 14, 44, 11, {opacity:.45});
      r(8, 14, 27, 11, {fill:'var(--acc)'}); break;
    case 'level':
      r(22, 5, 16, 29, {opacity:.45});
      g.appendChild(sv('rect', {x:22, y:19, width:16, height:15, rx:5,
        fill:'var(--acc)'})); break;
    case 'thermo':
      r(26, 4, 8, 20, {opacity:.35});
      g.appendChild(sv('rect', {x:27.5, y:13, width:5, height:12,
        fill:'var(--acc)'}));
      g.appendChild(sv('circle', {cx:30, cy:28, r:7, fill:'var(--acc)'})); break;
    case 'number': {
      var t = sv('text', {x:30, y:26, 'text-anchor':'middle', 'font-size':19,
        'font-weight':700, fill:'currentColor'});
      t.textContent = '42'; g.appendChild(t); break; }
    case 'spark':
      p('M8 26 L17 18 L24 22 L32 10 L40 16 L52 7',
        {stroke:'var(--acc)', 'stroke-width':2.6}); break;
    case 'state':
      r(11, 11, 38, 17, {fill:'var(--acc)', opacity:.75}); break;
  }
  return g;
}

function renderWidgetPicker(){
  var box = q('wpick');
  box.innerHTML = '';
  WIDGETS.forEach(function(w){
    var b = el('button');
    b.type = 'button';
    b.title = w.name + ' — ' + w.hint;
    b.className = (edCfg.w === w.id) ? 'on' : '';
    b.appendChild(miniIcon(w.id));
    b.appendChild(el('span', null, w.name));
    b.onclick = function(){
      edCfg.w = w.id;
      edPicked = true;
      renderWidgetPicker();
      applyFieldVisibility();
      refreshPreview();
    };
    box.appendChild(b);
  });
}

function findSig(ref){
  var out = null;
  DBC.m.forEach(function(m){
    m.s.forEach(function(s){ if(m.n + '.' + s.n === ref) out = s; });
  });
  return out;
}

function renderSignalList(filter){
  var box = q('slist');
  box.innerHTML = '';
  var f = (filter || '').toLowerCase(), shown = 0;

  DBC.m.forEach(function(m){
    m.s.forEach(function(s){
      var ref = m.n + '.' + s.n;
      if(f && ref.toLowerCase().indexOf(f) < 0 &&
         (s.u || '').toLowerCase().indexOf(f) < 0) return;
      if(shown >= 300) return;
      shown++;

      var row = el('div', 'row' + (edCfg.sig === ref ? ' on' : ''));
      var nm = el('div', 'sn', s.n);
      nm.title = ref;
      row.appendChild(nm);
      row.appendChild(el('div', 'sm', m.n + (s.u ? ' · ' + s.u : '')));
      row.appendChild(el('div', 'sv',
        LIVE[ref] !== undefined && LIVE[ref] !== '' ? LIVE[ref] : ''));
      row.onclick = function(){ chooseSignal(ref, s); };
      if(edCfg.sig === ref) box._sel = row;
      box.appendChild(row);
    });
  });

  /* The chosen row carries a highlight, but in a list of three hundred signals
     a highlight nobody can see reads as "nothing is selected". */
  if(box._sel){
    box.scrollTop = Math.max(0, box._sel.offsetTop - box.clientHeight / 2
                                + box._sel.offsetHeight / 2);
    box._sel = null;
  }

  if(!shown){
    box.appendChild(el('div', 'none', DBC.loaded
      ? 'No signal matches that.'
      : 'There is no frame map on the card, so there are no named signals to '
        + 'choose from. Add a DBC file and restart the logger.'));
  }
}

/* Picking a signal fills in everything the frame map already knows and
   suggests a shape - the difference between "choose a signal" and "answer six
   questions about a signal". Anything changed by hand is left alone. */
function chooseSignal(ref, s){
  edCfg.sig = ref;
  if(!edPicked) edCfg.w = guessWidget(ref, s);

  var lo = s.r ? s.lo : s.blo, hi = s.r ? s.hi : s.bhi;
  if(!isFinite(lo) || !isFinite(hi) || hi - lo > 1e7){ lo = 0; hi = 100; }
  edCfg.lo = lo;
  edCfg.hi = hi;
  edCfg.unit = s.u || '';
  edCfg.dec = isFinite(s.d) ? Math.min(s.d, 3) : null;
  if(!edCfg.label) edCfg.label = prettyName(ref);

  fieldsFromCfg();
  renderWidgetPicker();
  renderSignalList(q('sfilter').value);
  refreshPreview();
}

/* Which of the detail fields the chosen drawing actually reads. A state pill
   takes its text and its colour from the frame map's own VAL_ names - it never
   looks at a minimum, a maximum, a decimal count or a threshold - so offering
   those is offering six answers that change nothing. */
var WFIELDS = {
  /*          unit  dec   lo/hi  thresholds */
  state:    [ 0,    0,    0,     0 ],
  number:   [ 1,    1,    0,     1 ],
  compass:  [ 1,    1,    0,     1 ],
  spark:    [ 1,    1,    1,     1 ]
};
function applyFieldVisibility(){
  var f = WFIELDS[edCfg.w] || [1, 1, 1, 1];
  q('fw_unit').hidden   = !f[0];
  q('fw_dec').hidden    = !f[1];
  q('fw_lo').hidden     = !f[2];
  q('fw_hi').hidden     = !f[2];
  q('h_thresh').hidden  = !f[3];
  q('fw_thresh').hidden = !f[3];
  q('fw_lowbad').hidden = !f[3];

  var note = q('statenote');
  if(edCfg.w !== 'state'){ note.hidden = true; return; }
  var sg = findSig(edCfg.sig);
  note.hidden = false;
  note.innerHTML = (sg && sg.v && sg.v.length)
    ? 'The frame map names these states: <b>' + sg.v.join('</b> · <b>')
      + '</b>. They are shown as written, and coloured by what they say &mdash; '
      + 'anything reading fault, alarm or error goes red, ok or ready goes '
      + 'green. There is nothing to scale.'
    : 'This signal has no <code>VAL_</code> names in the frame map, so the '
      + 'number itself is shown. Adding a <code>VAL_</code> line to the '
      + '<code>.dbc</code> is what turns it into words.';
}

function fieldsFromCfg(){
  applyFieldVisibility();
  q('f_label').value = edCfg.label || '';
  q('f_unit').value  = edCfg.unit || '';
  q('f_dec').value   = (edCfg.dec === null || edCfg.dec === undefined) ? '' : edCfg.dec;
  q('f_lo').value    = edCfg.lo;
  q('f_hi').value    = edCfg.hi;
  q('f_warn').value  = (edCfg.warn === null || edCfg.warn === undefined) ? '' : edCfg.warn;
  q('f_crit').value  = (edCfg.crit === null || edCfg.crit === undefined) ? '' : edCfg.crit;
  q('f_lowbad').checked = !!edCfg.lowbad;
}
function cfgFromFields(){
  function opt(id){
    var v = q(id).value.trim();
    return v === '' ? null : parseFloat(v);
  }
  edCfg.label = q('f_label').value;
  edCfg.unit  = q('f_unit').value;
  var dv = q('f_dec').value.trim();
  edCfg.dec   = dv === '' ? null : Math.max(0, Math.min(6, parseInt(dv, 10) || 0));
  edCfg.lo    = parseFloat(q('f_lo').value) || 0;
  edCfg.hi    = parseFloat(q('f_hi').value);
  if(!isFinite(edCfg.hi)) edCfg.hi = edCfg.lo + 100;
  edCfg.warn   = opt('f_warn');
  edCfg.crit   = opt('f_crit');
  edCfg.lowbad = q('f_lowbad').checked ? 1 : 0;
}

/* The preview is the real widget fed the real value. Seeing the needle sit
   where it will sit is worth more than any description of a gauge. */
function refreshPreview(){
  var host = q('prevcell');
  host.innerHTML = '';
  host.className = 'cell';

  host.appendChild(el('div', 'cname', cellTitle(edCfg) || 'no signal chosen'));
  var w = makeWidget(edCfg);
  var gfx = el('div', 'gfx');
  if(w.node) gfx.appendChild(w.node);
  host.appendChild(gfx);

  var raw = LIVE[edCfg.sig];
  var v = (raw === undefined || raw === '') ? NaN : parseFloat(raw);
  /* With nothing live, park the needle where it can be seen rather than hard
     against the stop. */
  if(!isFinite(v) && edCfg.w !== 'state') v = edCfg.lo + (edCfg.hi - edCfg.lo) * 0.62;
  var z = zoneOf(edCfg, v);

  if(edCfg.w !== 'state'){
    host.appendChild(el('div', 'cval' + (w.node ? '' : ' big') + ' v-' + z,
                        fmtVal(v, decOf(edCfg))));
    host.appendChild(el('div', 'cunit', edCfg.unit || ''));
  }
  if(w.set) w.set(v, (raw === undefined ? String(Math.round(v)) : raw), z);
}

function openEditor(slot){
  loadDbc().then(function(){
    edSlot = slot;
    var have = CFG.cells[slot];
    edPicked = !!have;
    edCfg = have ? JSON.parse(JSON.stringify(have))
                 : {w:'number', sig:'', label:'', unit:'', lo:0, hi:100,
                    dec:null, warn:null, crit:null, lowbad:0};

    q('sheettitle').textContent = have ? 'Edit this cell' : 'Add a value';
    q('e_del').style.display = have ? '' : 'none';
    q('sfilter').value = '';
    renderSignalList('');
    renderWidgetPicker();
    fieldsFromCfg();
    refreshPreview();
    q('sheet').classList.add('on');
  });
}
function closeEditor(){ q('sheet').classList.remove('on'); }

/* ==========================================================================
 *  Transmit
 * ======================================================================== */
/* "620:620 mm|690:690 mm" -> [{v:620, l:'620 mm'}, ...]. Written by whoever set
   the value up; the logger only stores the string. */
function parseChoices(str){
  return String(str || '').split('|').filter(Boolean).map(function(p){
    var i = p.indexOf(':');
    var v = parseFloat(i < 0 ? p : p.slice(0, i));
    return {v: isFinite(v) ? v : 0, l: (i < 0 ? p : p.slice(i + 1)) || String(v)};
  });
}
function dumpChoices(list){
  return list.map(function(c){ return trimNum(c.v) + ':' + c.l; }).join('|');
}

function paintArm(){
  var card = q('armcard');
  card.classList.toggle('on', ARMED);
  q('armstate').textContent = ARMED ? 'ARMED' : 'NOT ARMED';
  q('armbtn').textContent = ARMED ? 'DISARM' : 'ARM TRANSMIT';
  q('armsub').textContent = ARMED
    ? 'Disarms itself in ' + hms(ARMLEFT) + ' unless something is sent'
    : (CANTX ? 'Every Send button is disabled until you arm'
             : 'This build is listen-only — it cannot write to the bus at all');
  q('armfill').style.width =
    Math.max(0, Math.min(100, (ARMLEFT / 300) * 100)).toFixed(1) + '%';

  /* The pinned copy says the same thing in less room. */
  var st = q('armstick');
  st.classList.toggle('live', ARMED);
  q('stickstate').textContent = ARMED ? 'ARMED' : 'NOT ARMED';
  q('stickbtn').textContent   = ARMED ? 'DISARM' : 'ARM TRANSMIT';
  q('sticksub').textContent   = ARMED ? 'disarms in ' + hms(ARMLEFT) : '';
  if(TAB !== 'send') st.classList.remove('on');
}

function txRange(t){
  var dp = (t.dec === null || t.dec === undefined) ? 2 : t.dec;
  if(t.raw) return t.id + ' · ' + (t.data || 'no bytes');
  var s = sigName(t.sig);
  if(t.style === 'choice' || t.style === 'toggle' || t.style === 'enum') return s;
  return s + ' · ' + t.lo.toFixed(dp) + ' to ' + t.hi.toFixed(dp) +
         (t.unit ? ' ' + t.unit : '');
}

/* The input a value is picked with. This is the part worth getting right: a
   tyre size is not a number somebody should be typing next to a running
   machine, it is one of the four sizes the fleet uses. */
function buildInput(t){
  var wrap = el('div', 'inp');

  function options(list){
    var sel = el('select');
    sel.dataset.role = 'value';
    list.forEach(function(o){
      var e2 = el('option', null, o.l);
      e2.value = o.v;
      if(Math.abs(o.v - t.preset) < 1e-9) e2.selected = true;
      sel.appendChild(e2);
    });
    wrap.appendChild(sel);
    return wrap;
  }

  if(t.style === 'enum'){
    var s = findSig(t.sig);
    var labels = (s && s.v && s.v.length) ? s.v : null;
    if(labels){
      return options(labels.map(function(l, i){ return {v:i, l:l}; }));
    }
    /* The frame map has no labels for this signal after all - fall through to
       a plain number rather than showing an empty list. */
  } else if(t.style === 'choice'){
    var list = parseChoices(t.choices);
    if(list.length) return options(list);
  } else if(t.style === 'toggle'){
    var two = parseChoices(t.choices);
    if(two.length < 2) two = [{v:0, l:'Off'}, {v:1, l:'On'}];
    var seg = el('div', 'seg');
    var hidden = el('input');
    hidden.type = 'hidden';
    hidden.dataset.role = 'value';
    hidden.value = t.preset;
    two.slice(0, 2).forEach(function(o){
      var b = el('button', Math.abs(o.v - t.preset) < 1e-9 ? 'on' : '', o.l);
      b.dataset.opt = o.v;
      b.onclick = function(){
        hidden.value = o.v;
        Array.prototype.forEach.call(seg.children, function(x){
          x.classList.toggle('on', x === b);
        });
      };
      seg.appendChild(b);
    });
    wrap.appendChild(seg);
    wrap.appendChild(hidden);
    return wrap;
  } else if(t.style === 'slider'){
    var rng = el('input');
    rng.type = 'range';
    rng.min = t.lo; rng.max = t.hi;
    rng.step = t.step || 'any';
    rng.value = t.preset;
    rng.dataset.role = 'value';
    var out = el('div', 'rv', fmtVal(t.preset, t.dec === null ? 2 : t.dec) +
                              (t.unit ? ' ' + t.unit : ''));
    rng.oninput = function(){
      out.textContent = fmtVal(parseFloat(rng.value),
                               t.dec === null ? 2 : t.dec) +
                        (t.unit ? ' ' + t.unit : '');
    };
    wrap.appendChild(rng);
    wrap.appendChild(out);
    return wrap;
  }

  var inp = el('input');
  inp.type = 'number';
  inp.step = t.step || 'any';
  inp.min = t.lo; inp.max = t.hi;
  inp.value = t.preset;
  inp.dataset.role = 'value';
  wrap.appendChild(inp);
  wrap.appendChild(el('div', 'u', t.unit || ''));
  return wrap;
}

function renderSend(){
  var box = q('sendlist');
  box.innerHTML = '';
  var keys = Object.keys(CFG.tx).map(Number).sort(function(a,b){return a-b;});

  /* With nothing set up, the whole card stays away. A fresh logger's Send tab
     is the arm button and the one-off frame, and nothing else. */
  q('savedcard').hidden = keys.length === 0;

  /* Values that share a group belong to one frame and get one Send button.
     Everything else is a row of its own, exactly as before. */
  var units = [], at = {};
  keys.forEach(function(i){
    var t = CFG.tx[i];
    if(!t || !t.label) return;
    var g = t.group || 0;
    if(g && at[g] !== undefined){ units[at[g]].ids.push(i); return; }
    if(g) at[g] = units.length;
    units.push({g:g, ids:[i]});
  });

  function valueRow(i, withSend){
    var t = CFG.tx[i];
    var row = el('div', 'sendrow');
    row.dataset.cmd = i;

    var lbl = el('div', 'lbl', t.label);
    var sub = el('small', null, txRange(t));
    var mn = muxNote(t.sig);
    if(mn) sub.appendChild(el('em', null, ' · ' + mn));
    lbl.appendChild(sub);
    row.appendChild(lbl);

    if(!t.raw) row.appendChild(buildInput(t));

    if(withSend){
      var send = el('button', null, 'Send');
      send.dataset.role = 'send';
      row.appendChild(send);

      if(t.cyclic){
        var cyc = el('button', 'cyc', 'Repeat');
        cyc.dataset.role = 'cyc';
        cyc.title = 'Keep sending this every ' + t.cyclic + ' ms';
        row.appendChild(cyc);
      }
    }
    return row;
  }

  units.forEach(function(u){
    if(u.ids.length < 2){ box.appendChild(valueRow(u.ids[0], true)); return; }

    var wrap = el('div', 'sendgroup');
    wrap.dataset.ids = u.ids.join(',');
    var head = el('div', 'ghead');
    head.appendChild(el('b', null, msgOf(CFG.tx[u.ids[0]].sig)));
    head.appendChild(el('span', null, 'these ' + u.ids.length
                        + ' go out together, in one frame'));
    wrap.appendChild(head);
    u.ids.forEach(function(i){ wrap.appendChild(valueRow(i, false)); });

    var send = el('button', 'pri', 'Send all ' + u.ids.length);
    send.dataset.role = 'sendgroup';
    wrap.appendChild(send);
    box.appendChild(wrap);
  });

  q('sendnote').textContent = 'Values are written to the bus and appear in the '
    + 'recording with a TX: prefix on the message name.';
  paintSendState();
}

/* "Message.Signal" -> "Message" */
function msgOf(ref){
  var dot = String(ref || '').indexOf('.');
  return dot < 0 ? String(ref || '') : ref.slice(0, dot);
}

/* A signal that only exists under one multiplexor code cannot be written
   without that code. The logger writes it automatically; this says so, because
   a value silently gaining a companion is exactly the kind of thing somebody
   needs to be told once. */
function muxNote(ref){
  if(!ref || !DBC.m.length) return '';
  var mn = msgOf(ref), out = '';
  DBC.m.forEach(function(m){
    if(m.n !== mn) return;
    var target = null, sel = null;
    m.s.forEach(function(s){
      if(m.n + '.' + s.n === ref) target = s;
      if(s.mx === -2) sel = s;
    });
    if(target && sel && target.mx >= 0){
      out = 'sent with ' + sel.n + ' = ' + target.mx;
    }
  });
  return out;
}

function paintSendState(){
  var live = ARMED && CANTX;
  Array.prototype.forEach.call(q('sendlist').querySelectorAll('button'),
    function(b){
      if(b.dataset.opt !== undefined) return;      /* a two-state choice */
      b.disabled = !live;
      if(b.dataset.role === 'cyc'){
        var row = b.closest('.sendrow');
        var on = !!(CYC & (1 << (+row.dataset.cmd)));
        b.classList.toggle('on', on);
        b.textContent = on ? 'Repeating' : 'Repeat';
      }
    });
  q('rawsend').disabled = !live;
}

/* Reads whatever input a row is carrying, whichever of the five kinds it is. */
function rowValue(row){
  var inp = row.querySelector('[data-role=value]');
  var v = inp ? parseFloat(inp.value) : 0;
  return isFinite(v) ? v : 0;
}

q('sendlist').addEventListener('click', function(e){
  var b = e.target.closest('button');
  if(!b || b.disabled || b.dataset.opt !== undefined) return;

  if(b.dataset.role === 'sendgroup'){
    var wrap = b.closest('.sendgroup');
    var ids = wrap.dataset.ids.split(',');
    var vals = ids.map(function(i){
      return rowValue(wrap.querySelector('.sendrow[data-cmd="' + i + '"]'));
    });
    /* One request, so the logger queues the members back to back and nothing
       can slip between them and split the frame. */
    postForm('/api/tx/send', {cmds:ids.join(','), values:vals.join(',')})
      .then(pollDash);
    return;
  }

  var row = b.closest('.sendrow');
  var i = +row.dataset.cmd, t = CFG.tx[i];
  var inp = row.querySelector('[data-role=value]');
  var v = inp ? parseFloat(inp.value) : 0;

  if(b.dataset.role === 'send'){
    if(!t.raw && t.style !== 'choice' && t.style !== 'enum' &&
       t.style !== 'toggle' && (!isFinite(v) || v < t.lo || v > t.hi)){
      toast('Not sent', 'The value must be between ' + t.lo + ' and ' + t.hi, 'bad');
      return;
    }
    postForm('/api/tx/send', {cmd:i, value:isFinite(v) ? v : 0}).then(pollDash);
  } else if(b.dataset.role === 'cyc'){
    postForm('/api/tx/cyclic', {cmd:i, on:(CYC & (1 << i)) ? 0 : 1,
                                value:isFinite(v) ? v : 0}).then(pollDash);
  }
});

/* ---- setting the sendable values up ------------------------------------ */
var TXED = {};

function renderTxEdit(){
  var box = q('txeditlist');
  box.innerHTML = '';
  var keys = Object.keys(TXED).map(Number).sort(function(a,b){return a-b;});

  if(!keys.length){
    box.appendChild(el('div', 'hint',
      'Nothing set up yet. Press "Add a value" below.'));
  }

  /* Values of one message are boxed together, the same way the Send tab boxes
     them, because that is what they are on the wire: one frame. Seeing four
     cards in a row with four Remove buttons invites you to treat them as four
     independent things, which for a multiplexed set they are not. */
  var byMsg = {};
  keys.forEach(function(k){
    var mn = TXED[k].sig ? msgOf(TXED[k].sig) : '';
    if(mn) (byMsg[mn] = byMsg[mn] || []).push(k);
  });
  var wraps = {};

  keys.forEach(function(i){
    var t = TXED[i];
    var card = el('div', 'txcard');

    var mates = muxSiblings(i);
    var isMux = mates.length > 1;
    var mname = t.sig ? msgOf(t.sig) : '';
    var unit  = (mname && byMsg[mname] && byMsg[mname].length > 1)
                ? byMsg[mname] : null;

    /* The box, built once for the first value of the message. */
    if(unit && !wraps[mname]){
      var wrap = el('div', 'sendgroup txgroup');
      var gh = el('div', 'ghead');
      gh.appendChild(el('b', null, mname));

      var sel = '';
      if(isMux){
        DBC.m.forEach(function(m){
          if(m.n !== mname) return;
          m.s.forEach(function(sg2){ if(sg2.mx === -2) sel = sg2.n; });
        });
      }
      gh.appendChild(el('span', null, isMux
        ? 'these ' + unit.length + ' are payloads of one multiplexed frame, '
          + 'chosen by ' + (sel || 'a selector') + ' — they are added and '
          + 'removed together'
        : 'these ' + unit.length + ' go out together, in one frame'));

      var gdel = el('button', null, 'Remove all ' + unit.length);
      gdel.onclick = function(){
        unit.forEach(function(k){ delete TXED[k]; });
        renderTxEdit();
      };
      gh.appendChild(gdel);
      wrap.appendChild(gh);
      box.appendChild(wrap);
      wraps[mname] = wrap;
    }

    var head = el('div', 'head');
    head.appendChild(el('b', null, t.label || 'Untitled value'));

    /* One Remove per value, EXCEPT in a multiplexed set: there the only honest
       button is the group's, because keeping some payloads of a multiplexed
       command describes only part of it. A plain group may be split - you may
       genuinely want three of its four signals - so those keep theirs. */
    if(!isMux){
      var del = el('button', null, 'Remove');
      del.onclick = function(){
        delete TXED[i];
        renderTxEdit();
      };
      head.appendChild(del);
    }
    card.appendChild(head);

    /* What the set is gets said once, on the box's header, rather than being
       repeated on every card inside it. */

    var f = el('div', 'fields');
    function field(lbl, key, type, span, attrs){
      var box = el('div');
      if(span) box.style.gridColumn = 'span ' + span;
      var id = 'txf_' + key + '_' + i;
      var l = el('label', null, lbl); l.setAttribute('for', id);
      box.appendChild(l);
      var inp = el('input');
      inp.id = id;
      inp.type = type || 'text';
      if(attrs) Object.keys(attrs).forEach(function(k){ inp[k] = attrs[k]; });
      inp.value = (t[key] === null || t[key] === undefined) ? '' : t[key];
      inp.oninput = function(){
        t[key] = (type === 'number') ? parseFloat(inp.value) : inp.value;
        if(key === 'label') head.firstChild.textContent = inp.value || 'Untitled value';
      };
      box.appendChild(inp);
      f.appendChild(box);
      return inp;
    }
    function select(lbl, id, list, cur, onch, span){
      var box = el('div');
      if(span) box.style.gridColumn = 'span ' + span;
      var l = el('label', null, lbl); l.setAttribute('for', id);
      box.appendChild(l);
      var sel = el('select');
      sel.id = id;
      list.forEach(function(o){
        var e2 = el('option', null, o.l);
        e2.value = o.v;
        if(o.v === cur) e2.selected = true;
        sel.appendChild(e2);
      });
      sel.onchange = function(){ onch(sel.value); };
      box.appendChild(sel);
      f.appendChild(box);
      return sel;
    }

    field('Name', 'label', 'text', 2);

    var sigs = [{v:'', l:'— choose a signal —'}];
    DBC.m.forEach(function(m){
      m.s.forEach(function(s){
        sigs.push({v:m.n + '.' + s.n,
                   l:m.n + '.' + s.n + (s.u ? '  (' + s.u + ')' : '')});
      });
    });
    select('Signal', 'txsig' + i, sigs, t.sig, function(v){
      t.sig = v;
      t.raw = false;
      var s = findSig(v);
      if(s){
        /* The input must stop where the signal does, so the bit limits win
           over the file's annotation whenever they are narrower. */
        t.lo = Math.max(s.r ? s.lo : s.blo, s.blo);
        t.hi = Math.min(s.r ? s.hi : s.bhi, s.bhi);
        t.unit = s.u || '';
        t.dec = isFinite(s.d) ? Math.min(s.d, 3) : null;
        if(!isFinite(t.preset) || t.preset < t.lo || t.preset > t.hi) t.preset = t.lo;
        /* A signal the frame map gives names to is a list, not a number. */
        if(s.v && s.v.length) t.style = 'enum';
        else if(s.b === 1)    t.style = 'toggle';
      }
      /* Choosing one payload of a multiplexed frame by hand brings the rest
         with it, for the same reason removing one takes the rest away. */
      var mn = msgOf(v);
      /* On the card being edited too - fillFromMessage below skips refs that
         are already set up, so this one would otherwise never be marked. */
      t.mux = isMuxMsg(mn) ? 1 : 0;
      if(isMuxMsg(mn)){
        DBC.m.forEach(function(m2){
          if(m2.n === mn) fillFromMessage(m2);
        });
      }
      renderTxEdit();
    }, 2);

    var sg = findSig(t.sig);
    var styles = [
      {v:'number', l:'Type a number'},
      {v:'slider', l:'Slider'},
      {v:'choice', l:'Pick from a list I write'},
      {v:'toggle', l:'Two states'}
    ];
    if(sg && sg.v && sg.v.length){
      styles.splice(2, 0, {v:'enum', l:'Pick from the frame map’s own names'});
    }
    select('How the value is picked', 'txst' + i, styles, t.style || 'number',
           function(v){ t.style = v; renderTxEdit(); }, 2);

    if(t.style === 'number' || t.style === 'slider'){
      field('Minimum', 'lo', 'number', 0, {step:'any'});
      field('Maximum', 'hi', 'number', 0, {step:'any'});
      field('Step', 'step', 'number', 0, {step:'any'});
      field('Default', 'preset', 'number', 0, {step:'any'});
    } else if(t.style === 'enum'){
      var d2 = el('div');
      d2.style.gridColumn = 'span 2';
      d2.appendChild(el('label', null, 'From the frame map'));
      d2.appendChild(el('div', 'sub',
        sg && sg.v.length ? sg.v.join(' · ') : 'this signal has no names'));
      f.appendChild(d2);
    }

    card.appendChild(f);

    if(t.style === 'choice' || t.style === 'toggle'){
      var lim = (t.style === 'toggle') ? 2 : 8;
      card.appendChild(el('h4', null, t.style === 'toggle'
        ? 'The two states' : 'The list to pick from'));
      var opts = el('div', 'opts');
      var list = parseChoices(t.choices);
      if(!list.length){
        list = (t.style === 'toggle') ? [{v:0, l:'Off'}, {v:1, l:'On'}]
                                      : [{v:t.lo || 0, l:''}];
      }
      function sync(){
        t.choices = dumpChoices(list);
        if(list.length) t.preset = list[0].v;
      }
      list.forEach(function(o, k){
        var r = el('div', 'optrow');
        var vi = el('input');
        vi.type = 'number'; vi.step = 'any'; vi.value = o.v;
        vi.title = 'The value that goes on the bus';
        vi.oninput = function(){ o.v = parseFloat(vi.value) || 0; sync(); };
        var li = el('input');
        li.placeholder = 'What it is called' + (t.unit ? ' (e.g. 690 ' + t.unit + ')' : '');
        li.value = o.l;
        li.oninput = function(){ o.l = li.value.replace(/[|:]/g, ''); sync(); };
        r.appendChild(vi); r.appendChild(li);
        if(t.style === 'choice'){
          var rm = el('button', null, '×');
          rm.onclick = function(){ list.splice(k, 1); sync(); renderTxEdit(); };
          r.appendChild(rm);
        }
        opts.appendChild(r);
      });
      sync();
      card.appendChild(opts);

      if(t.style === 'choice' && list.length < lim){
        var add = el('button', 'reboot', 'Add an option');
        add.onclick = function(){
          list.push({v:list.length ? list[list.length - 1].v : 0, l:''});
          sync();
          renderTxEdit();
        };
        card.appendChild(add);
      }
    }

    var f2 = el('div', 'fields');
    f2.style.marginTop = '10px';
    var rid = 'txcyc' + i;
    var rd = el('div');
    rd.style.gridColumn = 'span 2';
    var rl = el('label', null, 'Repeat every, milliseconds (0 = send once)');
    rl.setAttribute('for', rid);
    rd.appendChild(rl);
    var ri = el('input');
    ri.id = rid; ri.type = 'number'; ri.min = 0; ri.step = 10;
    ri.value = t.cyclic || 0;
    ri.oninput = function(){ t.cyclic = parseInt(ri.value, 10) || 0; };
    rd.appendChild(ri);
    f2.appendChild(rd);

    /* Which other values this one leaves with. Offered only for signals of the
       same message, because a group is one frame and a frame is one message.

       NOT called `mates`. It was, and `var` is function-scoped, so this
       assignment reached back and overwrote the mux-sibling list the Remove
       button above had already closed over - a list that deliberately INCLUDES
       this card, where this one deliberately excludes it. Remove therefore
       deleted every value of the message except the one whose button was
       pressed, while its label, computed before the overwrite, still said the
       right number. On a plain four-signal message, pressing Remove threw away
       the other three and kept the one you asked it to delete. */
    var groupMates = Object.keys(TXED).map(Number).filter(function(k){
      return k !== i && TXED[k].sig && msgOf(TXED[k].sig) === msgOf(t.sig || '');
    });
    if(groupMates.length){
      var gd = el('div');
      gd.style.gridColumn = 'span 2';
      var gid = 'txgrp' + i;
      var gl = el('label', null, 'Sent together with');
      gl.setAttribute('for', gid);
      gd.appendChild(gl);
      var gs = el('select');
      gs.id = gid;
      var o0 = el('option', null, 'on its own, in its own frame');
      o0.value = '0';
      if(!t.group) o0.selected = true;
      gs.appendChild(o0);
      var o1 = el('option', null, 'the other ' + msgOf(t.sig) + ' values, in one frame');
      o1.value = 'g';
      if(t.group) o1.selected = true;
      gs.appendChild(o1);
      gs.onchange = function(){
        if(gs.value === '0'){ t.group = 0; }
        else {
          /* Join whatever group the others already form, or start one. */
          var g = 0;
          groupMates.forEach(function(k){ if(TXED[k].group) g = TXED[k].group; });
          if(!g) g = freeGroup();
          t.group = g;
          groupMates.forEach(function(k){ TXED[k].group = g; });
        }
        renderTxEdit();
      };
      gd.appendChild(gs);
      f2.appendChild(gd);
    } else if(t.group){
      t.group = 0;                    /* a group of one is not a group */
    }

    card.appendChild(f2);

    (wraps[mname] || box).appendChild(card);
  });
}

/* What a signal that is being SENT should look like, before anyone says
   otherwise. Same spirit as guessWidget() on the dashboard side: the frame map
   already knows most of it, so nobody should have to retype it. */
function txStyleFor(s, span){
  if(s.v && s.v.length) return 'enum';    /* the file already named the states */
  if(s.b === 1)         return 'toggle';
  /* A slider is only an input if its whole travel is somewhere you might want
     to be. A 32-bit uptime counter declared [0|4294967295] is a number box. */
  if(s.r && span > 0 && span <= 1e5) return 'slider';
  return 'number';
}
/* 1, 2 or 5 times a power of ten - the steps a person reads off a scale. */
function niceStep(x){
  if(!(x > 0)) return 1;
  var p = Math.pow(10, Math.floor(Math.log(x) / Math.LN10));
  var m = x / p;
  return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}
function txFromSignal(ref, s){
  var lo = Math.max(s.r ? s.lo : s.blo, s.blo);
  var hi = Math.min(s.r ? s.hi : s.bhi, s.bhi);
  var dec = isFinite(s.d) ? Math.min(s.d, 3) : 0;
  var span = hi - lo;
  /* One raw count where a person could actually use that many stops, and a
     round number they could read off a scale where they could not. */
  var step = dec > 0 ? Math.pow(10, -dec) : 1;
  if(span > 0 && span / step > 2000) step = niceStep(span / 200);
  return {label:prettyName(ref), sig:ref, unit:s.u || '', lo:lo, hi:hi,
          step:step, preset:(lo <= 0 && hi >= 0) ? 0 : lo, dec:dec,
          cyclic:0, group:0, raw:false, style:txStyleFor(s, span), choices:''};
}

/* The lowest group number nothing is using yet. */
function freeGroup(){
  var used = {};
  Object.keys(TXED).forEach(function(k){
    if(TXED[k].group) used[TXED[k].group] = 1;
  });
  for(var g = 1; g < 200; g++) if(!used[g]) return g;
  return 0;
}

/* Is this message one whose payload depends on a selector? */
function isMuxMsg(name){
  var out = false;
  DBC.m.forEach(function(m){ if(m.n === name && m.mux) out = true; });
  return out;
}

/* Every value set up against the same multiplexed message.

   A multiplexed frame's payloads are one thing wearing several faces: which
   one the frame carries depends on the selector, and the selector is written
   from whichever payload is being sent. Keeping two of five is keeping a
   half-described command - the operator sees "wheel diameter" and "amplitude"
   and no way to tell that three other opcodes exist. So they arrive together
   and they leave together. */
function muxSiblings(idx){
  var t = TXED[idx];
  var mn = (t && t.sig) ? msgOf(t.sig) : '';
  /* The value's own record first, the frame map only as a fallback. Asking the
     map alone made the set dissolve whenever the map was absent - which is the
     normal state of the desk tool before a .dbc has been loaded, and of the
     logger itself when the card has no frame map on it. */
  if(!mn || !(t.mux || isMuxMsg(mn))) return [idx];
  return Object.keys(TXED).map(Number).filter(function(k){
    return TXED[k].sig && msgOf(TXED[k].sig) === mn;
  });
}

/* Adds every signal of one message. Returns what it did, so the caller can
   report on one message or on all of them in one sentence. */
function fillFromMessage(m){
  var taken = {};
  Object.keys(TXED).forEach(function(k){ taken[TXED[k].sig] = 1; });

  /* Two shapes of message, and they need opposite treatment.

     MULTIPLEXED - the selector picks which payload the frame carries, so the
     payload signals are ALTERNATIVES. Each becomes a value of its own, and the
     selector is not offered at all: the logger writes it from the frame map
     whenever one of its payloads is sent, which is the only way it can be
     right every time.

     PLAIN, with more than one signal - the signals are all in the frame at
     once, so writing one on its own means writing the others too, whatever
     they happen to contain. They are grouped: one Send, one frame. */
  var group = (!m.mux && m.s.length > 1) ? freeGroup() : 0;

  var r = {added:0, full:false, selector:''};
  m.s.forEach(function(s){
    var ref = m.n + '.' + s.n;
    if(s.mx === -2){ r.selector = s.n; return; }    /* the selector itself */
    if(taken[ref]) return;
    var i = 0;
    while(TXED[i]) i++;
    if(i >= MAXSEND){ r.full = true; return; }
    var t = txFromSignal(ref, s);
    t.group = group;
    /* Carried on the value, not looked up later: the Remove button has to know
       these belong together even when no frame map is loaded to ask. */
    t.mux = m.mux ? 1 : 0;
    TXED[i] = t;
    r.added++;
  });
  /* A group of one is not a group - it happens when everything but one signal
     of a plain message was already set up. */
  if(group && r.added < 2){
    Object.keys(TXED).forEach(function(k){
      if(TXED[k].group === group) TXED[k].group = 0;
    });
  }
  return r;
}

function txFillFromMap(){
  var pick = q('txfillmsg').value;

  /* With a role set, only what this logger transmits: writing a frame the ECU
     is the one sending means two nodes talking over each other on one id. */
  var list;
  if(pick === 'all'){
    list = DBC.m.filter(function(m){
      return (!CFG.role || m.tx === CFG.role) && m.s.length;
    });
    if(!list.length){
      toast('Nothing to add', CFG.role
        ? 'Nothing in the frame map is sent by ' + CFG.role
        : 'The frame map has no messages', 'bad');
      return;
    }
  } else {
    var m = DBC.m[+pick];
    if(!m){ toast('Nothing chosen', 'Pick a message first', 'bad'); return; }
    list = [m];
  }

  var added = 0, full = false, sel = '', names = [];
  list.forEach(function(m){
    var r = fillFromMessage(m);
    added += r.added;
    full = full || r.full;
    if(r.selector) sel = r.selector;
    if(r.added) names.push(m.n);
  });

  renderTxEdit();
  if(full){
    toast('Room ran out', added + ' added — this build stores ' + MAXSEND
          + ' values in total', 'bad');
  } else if(!added){
    toast('Nothing to add', 'Everything those messages carry is already here',
          'ok');
  } else if(sel){
    toast('Filled from the frame map', added + ' value(s) added from '
          + names.join(', ') + '. ' + sel + ' is not in the list — it is '
          + 'written automatically, with the right code for whichever value '
          + 'you send', 'ok');
  } else {
    toast('Filled from the frame map', added + ' value(s) added from '
          + names.join(', ') + ' — check how each one is picked before you go '
          + 'out', 'ok');
  }
}

function openTxEdit(){
  return loadDbc().then(function(){
    TXED = JSON.parse(JSON.stringify(CFG.tx));

    var sel = q('txfillmsg');
    sel.innerHTML = '';
    if(!DBC.m.length){
      sel.appendChild(el('option', null, 'no frame map loaded'));
      sel.disabled = true;
      q('txfill').disabled = true;
    } else {
      sel.disabled = false;
      q('txfill').disabled = false;

      /* The whole lot in one press, which is what most people want: a bus
         usually has one or two command frames and no reason to add them one
         at a time. */
      var usable = DBC.m.filter(function(m){
        return (!CFG.role || m.tx === CFG.role) && m.s.length;
      });
      var all = el('option', null, CFG.role
        ? 'every message ' + CFG.role + ' sends  (' + usable.length + ')'
        : 'every message in the frame map  (' + usable.length + ')');
      all.value = 'all';
      sel.appendChild(all);

      /* This logger's own messages first, the rest marked with who sends them,
         because writing a frame somebody else already sends is nearly always a
         mistake - and blocking it outright would be wrong on a bus where the
         node is simply not powered. */
      var order = DBC.m.map(function(m, i){ return {m:m, i:i}; });
      if(CFG.role){
        order.sort(function(a, b){
          return (b.m.tx === CFG.role ? 1 : 0) - (a.m.tx === CFG.role ? 1 : 0);
        });
      }
      order.forEach(function(e2){
        var m = e2.m, mine = !CFG.role || m.tx === CFG.role;
        var o = el('option', null, m.n + '  ' + m.id + '  ('
                   + m.s.length + ' signal' + (m.s.length === 1 ? '' : 's') + ')'
                   + (mine ? '' : '  — sent by ' + (m.tx || 'someone else')));
        o.value = e2.i;
        sel.appendChild(o);
      });

      /* The button said "in this message" whatever the list was set to, which
         reads as a contradiction when the list says every message. */
      sel.onchange = function(){
        q('txfill').textContent =
          sel.value !== 'all'  ? 'Add every signal in this message' :
          CFG.role             ? 'Add every signal ' + CFG.role + ' sends'
                               : 'Add every signal in every message';
      };
      sel.onchange();
    }

    renderTxEdit();
    q('txsheet').classList.add('on');
  });
}

/* ==========================================================================
 *  Wiring
 * ======================================================================== */
q('tabs').addEventListener('click', function(e){
  var b = e.target.closest('button');
  if(b) showTab(b.dataset.tab);
});

q('customize').onclick = function(){ setEdit(true); };
q('donedit').onclick   = function(){ setEdit(false); };
q('colplus').onclick   = function(){ step('cols', 1); };
q('colminus').onclick  = function(){ step('cols', -1); };
q('rowplus').onclick   = function(){ step('rows', 1); };
q('rowminus').onclick  = function(){ step('rows', -1); };
q('fillmap').onclick   = function(){ fillCells('map'); };
q('fillbus').onclick   = function(){ fillCells('bus'); };

q('clearcfg').onclick = function(){
  if(!confirm('Remove every cell from the dashboard?\n\nValues on the Send tab '
              + 'are not affected.')) return;
  CFG.cells = {};
  renderGrid();
  markDirty();
};

/* Export and import are deliberately NOT dashboard-only. /dash.cfg is one file
   describing the whole setup - the grid AND the values that can be sent - so
   the same pair of buttons appears wherever the operator is standing, and both
   ends of it round-trip everything. Hooked by class, not id, because the same
   control exists in more than one place on the page. */
function exportSetup(){
  /* Downloads what the LOGGER holds, not what this browser thinks it holds, so
     an export is always exactly the file that is on the card. */
  fetch('/api/dash/cfg').then(function(r){ return r.text(); }).then(function(t){
    var a = el('a');
    a.href = URL.createObjectURL(new Blob([t], {type:'text/plain'}));
    a.download = 'dash.cfg';
    a.click();
    setTimeout(function(){ URL.revokeObjectURL(a.href); }, 2000);
  });
}
function importSetup(){ q('filepick').click(); }

document.querySelectorAll('.cfgexport').forEach(function(b){ b.onclick = exportSetup; });
document.querySelectorAll('.cfgimport').forEach(function(b){ b.onclick = importSetup; });

/* ---- which node this logger is, and where the frame map comes from -------
   Asked FIRST, because both Fill buttons are useless without it and useless in
   a way that is not obvious: they quietly offer the wrong half. It sat in the
   setup-file sheet once, which is the last thing anyone opens, so by the time
   you met it the work it would have saved was already done by hand. */
function roleName(){ return CFG.role || ''; }

function renderRoleBtn(){
  q('rolebtn').textContent = 'Role: ' + (CFG.role || 'none');
  q('rolebtn').classList.toggle('set', !!CFG.role);
}

function setRole(v){
  CFG.role = v || '';
  renderRoleBtn();
  renderRoleSheet();
  renderSend();          /* the Send tab's Fill list depends on it */
  saveCfg();
}

function renderRoleSheet(){
  var list = q('rolelist'), note = q('rolenote');
  list.innerHTML = '';

  var counts = {};
  DBC.m.forEach(function(m){ if(m.tx) counts[m.tx] = (counts[m.tx] || 0) + 1; });
  var nodes = DBC.nodes || [];

  /* Skip first and always available. On a machine that already works, none of
     the nodes in the file IS this logger, and that is the common case. */
  var skip = el('button', CFG.role ? null : 'pri',
                'Skip — I am only listening');
  skip.onclick = function(){ setRole(''); };
  list.appendChild(skip);

  nodes.forEach(function(n){
    var k = counts[n] || 0;
    var b = el('button', CFG.role === n ? 'pri' : null,
               n + '  (sends ' + k + ' message' + (k === 1 ? '' : 's') + ')');
    b.onclick = function(){ setRole(n); };
    list.appendChild(b);
  });

  if(!nodes.length){
    note.innerHTML = 'This frame map names no nodes in a <code>BU_</code> line, '
      + 'so there is nothing to choose from — both Fill buttons will offer '
      + 'everything.';
    return;
  }
  if(CFG.role){
    var mine = DBC.m.filter(function(m){ return m.tx === CFG.role; })
                    .map(function(m){ return m.n; });
    note.innerHTML = mine.length
      ? 'Fill on the Send tab offers <b>' + mine.join('</b>, <b>')
        + '</b>. Fill on the dashboard offers everything else.'
      : 'Nothing in this frame map is sent by <b>' + CFG.role + '</b>, so there '
        + 'is nothing for this logger to write.';
  } else {
    note.innerHTML = 'Skipped — both Fill buttons offer every message, and you '
      + 'sort out which is which. That is the right answer when you are '
      + 'recording a machine that already works.';
  }
}

function openRole(){
  /* Returns the promise: the sheet is only correct once the frame map has
     arrived, and callers - including the screenshot tool - have to be able to
     wait for that rather than catching it half-built. */
  return loadDbc().then(function(){
    renderRoleSheet();
    q('rolesheet').classList.add('on');
  });
}

q('rolebtn').onclick   = openRole;
q('role_close').onclick = function(){ q('rolesheet').classList.remove('on'); };
q('rolesheet').onclick  = function(e){
  if(e.target === q('rolesheet')) q('rolesheet').classList.remove('on');
};

q('dbcbtn').onclick = function(){ q('dbcpick').click(); };

q('dbcpick').onchange = function(){
  var f = q('dbcpick').files[0];
  if(!f) return;
  q('dbcpick').value = '';

  /* Multipart, so the logger can stream it to the card a chunk at a time - a
     real machine's .dbc is ninety kilobytes and would not fit comfortably in
     its heap all at once. */
  var fd = new FormData();
  fd.append('file', f, f.name);

  toast('Sending the frame map', f.name + ' — ' + Math.round(f.size / 1024)
        + ' KB', 'ok');

  fetch('/api/dbc', {method:'POST', body:fd})
    .then(function(r){ return r.json(); })
    .then(function(d){
      if(!d.ok){
        toast('Frame map not loaded', d.err || 'the logger refused it', 'bad');
        return;
      }
      return loadDbc(1).then(function(){
        /* A different frame map is a different bus. Anything the new one
           cannot account for goes, rather than sitting there as a cell that
           reads "unknown" for ever - and it has to go here as well as on the
           logger, because this copy is what gets saved back over the setup
           file a moment later. */
        var gone = dropUnresolved();
        toast('Frame map loaded', d.messages + ' message(s), ' + d.signals
              + ' signal(s)'
              + (d.errors ? ', ' + d.errors + ' line(s) unreadable' : '')
              + (gone ? ' — ' + gone + ' cell(s) and value(s) from the old map '
                        + 'removed' : ''), 'ok');
        renderGrid();
        renderSend();
        var done = gone ? saveCfg() : Promise.resolve();
        /* Straight into the question that has to be answered before either
           Fill button is worth pressing. */
        return done.then(openRole);
      });
    })
    .catch(function(){
      toast('Frame map not loaded', 'the logger did not answer', 'bad');
    });
};

q('setupbtn').onclick = function(){
  /* Counted from the logger's own copy, not this browser's, so the sheet
     reports what would actually be exported. */
  Promise.all([loadDbc(), fetch('/api/dash/cfg').then(function(r){
    return r.text();
  })]).then(function(res){
    var t = res[1];
    var c = (t.match(/^cell /gm) || []).length;
    var v = (t.match(/^send /gm) || []).length;
    q('cfgsummary').textContent = c + ' dashboard cell' + (c === 1 ? '' : 's')
                                + ' · ' + v + ' sendable value' + (v === 1 ? '' : 's');
    q('cfgbytes').textContent = t.length + ' bytes of /dash.cfg';
    q('cfgsheet').classList.add('on');
  });
};
q('cfg_close').onclick = function(){ q('cfgsheet').classList.remove('on'); };
q('cfgsheet').onclick = function(e){
  if(e.target === q('cfgsheet')) q('cfgsheet').classList.remove('on');
};

q('filepick').onchange = function(){
  var f = q('filepick').files[0];
  if(!f) return;
  f.text().then(function(t){
    return fetch('/api/dash/cfg', {method:'POST', body:t});
  }).then(function(){
    q('filepick').value = '';
    return loadCfg();   /* redraws the grid AND the Send tab */
  }).then(function(){
    var cells = Object.keys(CFG.cells).length,
        sends = Object.keys(CFG.tx).length;
    q('cfgsheet').classList.remove('on');
    toast('Imported', cells + ' dashboard cell(s) and ' + sends
          + ' sendable value(s) are now on the card', 'ok');
  });
};

q('sfilter').oninput = function(){ renderSignalList(q('sfilter').value); };
['f_label','f_unit','f_dec','f_lo','f_hi','f_warn','f_crit'].forEach(function(id){
  q(id).oninput = function(){ cfgFromFields(); refreshPreview(); };
});
q('f_lowbad').onchange = function(){ cfgFromFields(); refreshPreview(); };

q('e_cancel').onclick = closeEditor;
q('e_del').onclick = function(){
  removeCell(edSlot);
  closeEditor();
  renderGrid();
  markDirty();
};
q('e_ok').onclick = function(){
  cfgFromFields();
  if(!edCfg.sig){
    toast('Nothing chosen', 'Pick a signal for this cell first', 'bad');
    return;
  }
  CFG.cells[edSlot] = edCfg;
  closeEditor();
  renderGrid();
  markDirty();
};
q('sheet').onclick = function(e){ if(e.target === q('sheet')) closeEditor(); };

function toggleArm(){
  if(!CANTX){
    toast('Listen-only', 'This build cannot write to the bus. Set '
          + 'CAN_LISTEN_ONLY to 0 in config.h and reflash.', 'bad');
    return;
  }
  postForm('/api/tx/arm', {on:ARMED ? 0 : 1}).then(pollDash);
}
q('armbtn').onclick   = toggleArm;
q('stickbtn').onclick = toggleArm;

/* Show the pinned bar only once the real card has scrolled off. Watching the
   card itself rather than a scroll offset means it stays right whatever else
   is above it on the page. */
if(window.IntersectionObserver){
  new IntersectionObserver(function(es){
    var gone = es[0] && !es[0].isIntersecting;
    q('armstick').classList.toggle('on', !!gone && TAB === 'send');
  }, {threshold:0}).observe(q('armcard'));
}
q('rawsend').onclick = function(){
  var id = q('rawid').value.trim();
  var data = q('rawdata').value.replace(/[^0-9a-fA-F]/g, '');
  if(!id){ toast('Not sent', 'Enter an identifier', 'bad'); return; }
  if(data.length % 2){ toast('Not sent', 'The payload needs whole bytes', 'bad'); return; }
  postForm('/api/tx/send', {id:id, data:data}).then(pollDash);
};
q('editsend').onclick = openTxEdit;
q('tx_cancel').onclick = function(){ q('txsheet').classList.remove('on'); };
q('txfill').onclick = txFillFromMap;

q('tx_add').onclick = function(){
  var i = 0;
  while(TXED[i] && i < MAXSEND) i++;
  if(i >= MAXSEND){
    toast('Full', 'This build stores ' + MAXSEND + ' saved values', 'bad');
    return;
  }
  TXED[i] = {label:'New value', sig:'', unit:'', lo:0, hi:100, step:1,
             preset:0, dec:null, cyclic:0, group:0, raw:false, style:'number',
             choices:''};
  renderTxEdit();
};
q('tx_ok').onclick = function(){
  CFG.tx = {};
  Object.keys(TXED).forEach(function(k){
    var t = TXED[k];
    if(t && t.label && (t.sig || t.raw)) CFG.tx[k] = t;
  });
  q('txsheet').classList.remove('on');
  renderSend();
  saveCfg();
};

q('btn').onclick = function(){
  fetch(q('btn').className === 'stop' ? '/api/stop' : '/api/start', {method:'POST'});
  q('btn').textContent = '...';
};
q('rebootbtn').onclick = function(){
  if(!confirm('Restart the logger?\n\nAny running recording is closed and '
              + 'saved first. The next recording goes to a new file.')) return;
  fetch('/api/reboot', {method:'POST'});
  q('conn').textContent = 'restarting...';
  setTimeout(function(){ location.reload(); }, 8000);
};

window.addEventListener('hashchange', function(){
  showTab(location.hash.slice(1));
});
loadCfg().then(function(){ showTab(location.hash.slice(1) || 'dash'); });
</script>
</body></html>
)HTML";

/* ==========================================================================
 *  The parts, in order.
 * ======================================================================== */
const char *const PAGE_PARTS[] = {
  PAGE_1, PAGE_2, PAGE_3, PAGE_4, PAGE_5
};
const uint8_t PAGE_PART_COUNT = sizeof(PAGE_PARTS) / sizeof(PAGE_PARTS[0]);
