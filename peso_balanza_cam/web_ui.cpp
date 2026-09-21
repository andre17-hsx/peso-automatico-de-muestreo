// ===========================================================================
//  web_ui.cpp  -  Servidor web:  panel + historial + calibracion + bus
//
//  Rutas (cam solo si ENABLE_OCR, bus solo si ENABLE_SNIFFER):
//    /                 panel
//    /api              JSON estado en vivo (poll 1 s)
//    /weighings        JSON historial de pesajes capturados
//    /weighings.csv    historial en CSV (para descargar)
//    /weighings/clear  POST -> borra el historial
//    /targets          ?malla=200&bin=800 -> pone los objetivos de agrupacion
//    /snapshot.jpg     foto actual (gris)                          [OCR]
//    /ocr_debug.jpg    foto con las zonas dibujadas                [OCR]
//    /sniffer          16 bytes de RAM del display                 [SNIFFER]
//    /sniffer/raw      volcado crudo de flancos                    [SNIFFER]
//    /config           calibracion
// ===========================================================================
#include "web_ui.h"
#include "config.h"
#include "seg7.h"
#include "ocr7seg.h"
#include "sniffer_tm1640.h"
#include "weighlog.h"
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

static WebServer* S = nullptr;

// numero JSON: valor con 'dec' decimales, o null si es NaN
static void jnum(String& h, float v, int dec) {
  if (isnan(v)) h += "null"; else h += String(v, dec);
}

// texto JSON entre comillas (escapa " y \ ; los caracteres de control ya se
// filtran al guardarlo, ver cleanInfo)
static void jstr(String& h, const String& s) {
  h += '"';
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') h += '\\';
    h += c;
  }
  h += '"';
}

// Datos de la jornada que teclea el operario: nombre de sector y nº de piscina.
// Van en NVS aparte ("sess") -> sobreviven a reinicios Y a "Borrar todo" (igual
// que los objetivos de malla/BIN).  Solo los toca el hilo del servidor web.
// Son de SESION (no van por pesaje): el CSV repite el valor que haya al exportar.
static String g_sector, g_piscina;
#define SECTOR_MAX_BYTES   64
#define PISCINA_MAX_BYTES  24

static String cleanInfo(const String& in, size_t maxBytes) {
  String o; o.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x20 || c == 0x7f) continue;        // sin saltos de linea / tabs / control
    if (c == ';') c = ',';                      // ';' es el separador del CSV
    if (c == '"') c = '\'';
    o += (char)c;
  }
  o.trim();
  if (o.length() > maxBytes) {                  // recorta sin partir un caracter UTF-8 (ñ, á...)
    size_t cut = maxBytes;
    while (cut > 0 && ((uint8_t)o[cut] & 0xC0) == 0x80) cut--;
    o = o.substring(0, cut);
  }
  return o;
}

static void infoLoad() {
  Preferences pr;
  if (!pr.begin("sess", false)) return;
  g_sector  = cleanInfo(pr.getString("sector", ""), SECTOR_MAX_BYTES);
  g_piscina = cleanInfo(pr.getString("pisc",   ""), PISCINA_MAX_BYTES);
  pr.end();
}

static void infoSave() {
  Preferences pr;
  if (!pr.begin("sess", false)) return;
  pr.putString("sector", g_sector);
  pr.putString("pisc",   g_piscina);
  pr.end();
}

// epoch UTC -> "2026-09-08 09:03:00" en la zona horaria local (TZ_POSIX).
// El dato se guarda en UTC; esto es solo para MOSTRARLO.  Vacio si epoch <= 0.
static void isoLocal(long epoch, char* out, size_t n) {
  out[0] = 0;
  if (epoch <= 0) return;
  time_t t = (time_t)epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

#if ENABLE_SNIFFER
// mascara 7-seg estandar (a=bit0 .. g=bit6) y punto, a partir del byte crudo
static uint8_t stdMask(uint8_t raw) {
  uint8_t m = 0;
  for (int k = 0; k < 7; k++) if ((raw >> snf.segBit[k]) & 1) m |= (1 << k);
  return m;
}
static bool stdDp(uint8_t raw) { return (raw >> snf.segBit[7]) & 1; }

// Añade  "mask":[16],"dp":[16]  con los 16 digitos EN ORDEN DE LECTURA
// (PESO 0-4, PRECIO 5-9, TOTAL 10-15), aplicando el mapa snf.*Addr -> asi la
// pantalla del panel coincide 1:1 con lo que se ve en la balanza.
static void appendDisp(String& h, const SnifState& s) {
  const int8_t* maps[3] = { snf.pesoAddr, snf.precioAddr, snf.totalAddr };
  int           lens[3] = { snf.pesoLen,  snf.precioLen,  snf.totalLen  };
  const int     wid[3]  = { 5, 5, 6 };
  for (int pass = 0; pass < 2; pass++) {            // 0 = mask, 1 = dp
    h += (pass == 0) ? "\"mask\":[" : ",\"dp\":[";
    bool first = true;
    for (int f = 0; f < 3; f++) {
      int lead = wid[f] - lens[f]; if (lead < 0) lead = 0;
      for (int i = 0; i < wid[f]; i++) {
        if (!first) h += ',';
        first = false;
        int mi = i - lead;
        uint8_t raw = 0;
        if (mi >= 0 && mi < lens[f]) {
          int a = maps[f][mi];
          if (a >= 0 && a < 16) raw = s.shadow[a];
        }
        h += (pass == 0) ? (int)stdMask(raw) : (stdDp(raw) ? 1 : 0);
      }
    }
    h += ']';
  }
}
#endif

// --------------------------------------------------------------------------
static const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html><html lang=es>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name=theme-color media="(prefers-color-scheme:light)" content="#e7edf7">
<meta name=theme-color media="(prefers-color-scheme:dark)" content="#0b1220">
<title>Balanza</title>
<script>(function(){try{var t=localStorage.getItem('bz-theme');if(t)document.documentElement.dataset.theme=t}catch(e){}})()</script>
<style>
 :root{
  color-scheme:light dark;
  --bg1:#eef2f9; --bg2:#dde6f1; --glow:rgba(96,132,205,.20);
  --tx:#18202e; --tx2:#54607a; --tx3:#8894aa;
  --glass:rgba(255,255,255,.60); --solid:rgba(255,255,255,.80);
  --brd:rgba(255,255,255,.78); --line:rgba(22,32,58,.11);
  --acc:#0d8ba6; --accs:rgba(13,139,166,.13);
  --ok:#1c9c53; --ok-bg:rgba(28,156,83,.10); --amber:#b9770f; --danger:#d13e39; --danger-bg:rgba(209,62,57,.10);
  --led:#ff4326; --ledoff:rgba(255,67,38,.13);
  --scr1:#16181c; --scr2:#0a0b0d; --scrbrd:rgba(255,255,255,.07);
  --rowbg:#f7f9fc;
  --sh:0 8px 30px rgba(42,62,108,.14);
 }
 @media (prefers-color-scheme:dark){:root:not([data-theme=light]){
  --bg1:#0b1220; --bg2:#141c2f; --glow:rgba(255,92,64,.13);
  --tx:#e7ecf4; --tx2:#9aa6bc; --tx3:#69748a;
  --glass:rgba(255,255,255,.055); --solid:rgba(22,29,46,.72);
  --brd:rgba(255,255,255,.12); --line:rgba(255,255,255,.08);
  --acc:#5ac8d8; --accs:rgba(90,200,216,.15);
  --ok:#3ddc84; --ok-bg:rgba(61,220,132,.14); --amber:#f4b740; --danger:#ff5a52; --danger-bg:rgba(255,90,82,.14);
  --ledoff:rgba(255,67,38,.10);
  --scr1:#14161c; --scr2:#090a0d; --scrbrd:rgba(255,255,255,.09);
  --rowbg:#141b2b;
  --sh:0 12px 36px rgba(0,0,0,.42);
 }}
 :root[data-theme=dark]{
  --bg1:#0b1220; --bg2:#141c2f; --glow:rgba(255,92,64,.13);
  --tx:#e7ecf4; --tx2:#9aa6bc; --tx3:#69748a;
  --glass:rgba(255,255,255,.055); --solid:rgba(22,29,46,.72);
  --brd:rgba(255,255,255,.12); --line:rgba(255,255,255,.08);
  --acc:#5ac8d8; --accs:rgba(90,200,216,.15);
  --ok:#3ddc84; --ok-bg:rgba(61,220,132,.14); --amber:#f4b740; --danger:#ff5a52; --danger-bg:rgba(255,90,82,.14);
  --ledoff:rgba(255,67,38,.10);
  --scr1:#14161c; --scr2:#090a0d; --scrbrd:rgba(255,255,255,.09);
  --rowbg:#141b2b;
  --sh:0 12px 36px rgba(0,0,0,.42);
 }
 *{box-sizing:border-box}
 [hidden]{display:none!important}
 html,body{margin:0}
 body{font-family:-apple-system,"Segoe UI",Roboto,system-ui,sans-serif;color:var(--tx);
   background:var(--bg1);font-variant-numeric:tabular-nums;
   -webkit-tap-highlight-color:transparent;min-height:100vh}
 body::before{content:"";position:fixed;inset:0;z-index:-1;
   background:radial-gradient(58% 42% at 82% -4%,var(--glow),transparent 72%),
     linear-gradient(180deg,var(--bg1),var(--bg2))}
 .bar{position:sticky;top:0;z-index:9;display:flex;align-items:center;gap:10px;
   padding:11px 15px;background:var(--glass);border-bottom:1px solid var(--line);
   -webkit-backdrop-filter:blur(16px) saturate(1.4);backdrop-filter:blur(16px) saturate(1.4)}
 .bar .ttl{font-weight:650;font-size:15px}
 .ib{margin-left:auto;width:33px;height:33px;display:grid;place-items:center;
   border-radius:10px;border:1px solid var(--brd);background:var(--solid);
   color:var(--tx2);cursor:pointer;transition:transform .12s}
 .ib:active{transform:scale(.93)}
 .ib svg{width:16px;height:16px;display:block}
 .live{width:8px;height:8px;border-radius:50%;background:var(--tx3);transition:.3s;flex:0 0 auto}
 .live.up{background:var(--ok);box-shadow:0 0 9px var(--ok)}
 .wrap{padding:16px;max-width:620px;margin:0 auto}
 .lbl{font-size:10px;color:var(--tx3);letter-spacing:.15em;font-weight:700}
 a{color:var(--acc);text-decoration:none}
 button{font:inherit;padding:8px 14px;border-radius:10px;border:1px solid var(--brd);
   background:var(--solid);color:var(--tx);cursor:pointer;transition:transform .12s}
 button:active{transform:scale(.97)}
 button.danger{border-color:var(--danger);color:var(--danger);background:transparent}
 :focus-visible{outline:2px solid var(--acc);outline-offset:2px}
 /* ---- pantalla LED (oscura en ambos temas: es una pantalla LED real) ---- */
 .screen{border:1px solid var(--scrbrd);border-radius:18px;padding:16px 18px 18px;
   background:linear-gradient(180deg,var(--scr1),var(--scr2));
   box-shadow:inset 0 1px 0 rgba(255,255,255,.05),0 16px 44px rgba(0,0,0,.34)}
 .fld{margin-top:14px}.fld:first-child{margin-top:0}
 .fhead{display:flex;align-items:center;font-size:9.5px;letter-spacing:.17em;
   color:#d1553d;font-weight:800;margin-bottom:5px}
 .fhead .u{color:#7d818a;margin-left:6px;font-weight:700}
 .ledrow{display:flex;align-items:flex-end;gap:1px;overflow-x:auto;
   filter:drop-shadow(0 0 6px rgba(255,67,38,.28))}
 .ledrow::-webkit-scrollbar{display:none}
 .d{flex:0 0 auto}
 .d.xl{height:86px}.d.md{height:44px}
 .d .on{fill:var(--led)}
 .d .off{fill:var(--ledoff)}
 .lamp{margin-left:auto;display:flex;align-items:center;gap:7px;font-size:9.5px;
   letter-spacing:.13em;color:#6a7078;font-weight:800}
 .lamp .dot{width:9px;height:9px;border-radius:50%;background:#2b2e34;transition:.2s}
 .lamp.on{color:var(--ok)}.lamp.on .dot{background:var(--ok);box-shadow:0 0 10px var(--ok)}
 .grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px 18px}
 .dv{height:1px;background:var(--scrbrd);margin:14px 0 2px}
 .bigtxt{font-size:15vw;text-align:center;line-height:1;margin:8px 0;color:var(--led)}
 /* ---- estado ---- */
 .prow{display:flex;justify-content:center;margin-top:16px}
 .pill{display:inline-flex;align-items:center;gap:8px;padding:7px 15px;border-radius:999px;
   font-size:13px;font-weight:650;background:var(--glass);border:1px solid var(--brd);
   box-shadow:var(--sh);-webkit-backdrop-filter:blur(10px);backdrop-filter:blur(10px)}
 .pill .pd{width:7px;height:7px;border-radius:50%;background:var(--tx3);flex:0 0 auto}
 .pill.weigh{color:var(--amber)}.pill.weigh .pd{background:var(--amber);box-shadow:0 0 8px var(--amber)}
 .pill.stable{color:var(--ok)}.pill.stable .pd{background:var(--ok);box-shadow:0 0 8px var(--ok)}
 .sub{text-align:center;color:var(--tx2);font-size:11.5px;margin-top:8px}
 /* ---- objetivos: malla / bin ---- */
 .targets{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px}
 .targets+.targets{margin-top:10px}
 .tgt.txt input{font-size:16px}
 .tgt{padding:12px 14px;border-radius:15px;background:var(--glass);border:1px solid var(--brd);
   box-shadow:var(--sh);-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px);
   transition:background .2s,border-color .2s}
 .tgt label{display:block;font-size:9.5px;color:var(--tx3);letter-spacing:.1em;
   text-transform:uppercase;font-weight:700;margin-bottom:8px}
 .tgt .row{display:flex;align-items:baseline;gap:6px}
 .tgt input{width:100%;min-width:0;font:inherit;font-size:19px;font-weight:750;color:var(--tx);
   background:transparent;border:none;border-bottom:2px solid var(--line);padding:2px 0 6px;
   -moz-appearance:textfield}
 .tgt input::-webkit-outer-spin-button,.tgt input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
 .tgt input:focus{outline:none;border-color:var(--acc)}
 .tgt .row i{font-size:12px;font-weight:600;color:var(--tx3);font-style:normal}
 .tgtsub{margin-top:8px;font-size:11.5px;color:var(--tx2);min-height:14px}
 .tgt.pulse{animation:flashok .9s ease-out}
 @keyframes flashok{from{box-shadow:0 0 0 3px var(--ok),var(--sh)}
   to{box-shadow:0 0 0 0 transparent,var(--sh)}}
 @media (max-width:380px){.targets{grid-template-columns:1fr}}
 /* ---- pop-up: malla/BIN completada ---- */
 .gpop{position:fixed;inset:0;z-index:50;display:flex;align-items:center;justify-content:center;
   padding:20px;background:rgba(0,0,0,.55);opacity:0;pointer-events:none;transition:opacity .25s}
 .gpop.show{opacity:1;pointer-events:auto}
 .gpop .card{max-width:340px;width:100%;padding:26px 24px;border-radius:22px;text-align:center;
   background:var(--solid);border:1px solid var(--ok);box-shadow:0 20px 60px rgba(0,0,0,.5);
   transform:scale(.9);transition:transform .25s}
 .gpop.show .card{transform:scale(1)}
 .gpop .ico{width:52px;height:52px;margin:0 auto 12px;border-radius:50%;background:var(--ok-bg);
   color:var(--ok);display:grid;place-items:center}
 .gpop .ico svg{width:28px;height:28px}
 .gpop .ttl2{font-size:17px;font-weight:750;color:var(--tx)}
 .gpop .val{font-size:32px;font-weight:800;color:var(--ok);margin:10px 0 2px}
 .gpop .val i{font-size:15px;font-weight:600;color:var(--tx3);font-style:normal;margin-left:4px}
 .gpop .hint2{font-size:12px;color:var(--tx3);margin-top:14px}
 /* ---- historial: cabeceras de grupo (BIN / malla) ---- */
 .ghdr{display:flex;justify-content:space-between;align-items:baseline;gap:8px;
   padding:9px 14px;font-size:11px;font-weight:750;letter-spacing:.04em;
   color:var(--tx2);background:var(--accs);border-top:1px solid var(--line)}
 .whist .ghdr:first-child{border-top:none}
 .ghdr-v{color:var(--tx3);font-weight:650}
 .ghdr-m{padding-left:26px;background:transparent;font-weight:650;font-size:10.5px}
 /* ---- ultimo pesaje ---- */
 .last{display:none;margin-top:18px;padding:15px 16px;border-radius:15px;
   background:var(--glass);border:1px solid var(--brd);border-left:3px solid var(--acc);
   box-shadow:var(--sh);-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px)}
 .last.new{animation:flash 1s ease-out}
 @keyframes flash{from{box-shadow:0 0 0 3px var(--acc),var(--sh)}
   to{box-shadow:0 0 0 0 transparent,var(--sh)}}
 .last .top{display:flex;align-items:baseline;gap:9px}
 .last .big{font-size:27px;font-weight:750}
 .last .k{color:var(--tx3);font-size:13px}
 .last .chip{margin-left:auto;font-size:11px;color:var(--tx2);background:var(--accs);
   border:1px solid var(--line);border-radius:8px;padding:2px 9px}
 .last .meta{color:var(--tx2);font-size:12.5px;margin-top:6px}
 /* ---- peso total acumulado ---- */
 .totbox{margin-top:14px;display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;
   padding:13px 16px;border-radius:15px;background:var(--glass);border:1px solid var(--brd);
   box-shadow:var(--sh);-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px)}
 .totbox .totlbl{width:100%;font-size:10px;color:var(--tx3);letter-spacing:.15em;font-weight:700}
 .totval{font-size:24px;font-weight:750}
 .totval i{font-size:13px;font-weight:600;color:var(--tx3);font-style:normal;margin-left:4px}
 .totn{margin-left:auto;font-size:12px;color:var(--tx2)}
 /* ---- historial (lista deslizable) ---- */
 .hh{display:flex;align-items:center;gap:9px;margin:24px 0 8px}
 .hh .cnt{font-size:11px;background:var(--accs);border:1px solid var(--line);
   border-radius:999px;padding:2px 10px;color:var(--tx2)}
 .hact{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}
 .hact button,.hact .lnk{font-size:12.5px;padding:7px 12px;border-radius:9px;
   border:1px solid var(--brd);background:var(--solid);color:var(--tx);
   text-decoration:none;cursor:pointer;white-space:nowrap;transition:transform .12s}
 .hact button:active,.hact .lnk:active{transform:scale(.96)}
 .hact button.danger{border-color:var(--danger);color:var(--danger);background:transparent}
 .confirm{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:10px;
   padding:10px 13px;border-radius:11px;background:var(--danger-bg);
   border:1px solid var(--danger);font-size:12.5px;color:var(--tx)}
 .confirm .sp{flex:1 1 20px}
 .confirm button{font-size:12.5px;padding:6px 12px;border-radius:8px;
   border:1px solid var(--brd);background:var(--solid);color:var(--tx);cursor:pointer}
 .confirm button.danger{border-color:var(--danger);background:var(--danger);color:#fff}
 .hint{font-size:11px;color:var(--tx3);margin:0 2px 8px}
 .whist{border:1px solid var(--brd);border-radius:14px;overflow:hidden;
   background:var(--glass);box-shadow:var(--sh);
   -webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px)}
 .wrow{position:relative;overflow:hidden;border-top:1px solid var(--line)}
 .wrow:first-child{border-top:none}
 .wr-del{position:absolute;top:0;bottom:0;right:0;width:112px;
   display:flex;align-items:center;justify-content:center;gap:6px;
   background:var(--danger);color:#fff;font-weight:650;font-size:13px;
   border:0;cursor:pointer;opacity:0;transition:opacity .15s}
 .wrow.open .wr-del,.wrow.drag .wr-del{opacity:1}
 .wr-front{position:relative;background:var(--rowbg);padding:11px 14px;
   transition:transform .2s cubic-bezier(.2,.7,.25,1);touch-action:pan-y;
   -webkit-user-select:none;user-select:none}
 .wrow.open .wr-front{transform:translateX(-108px)}
 .wr-top{display:flex;align-items:baseline;gap:8px}
 .wr-n{font-size:11px;color:var(--tx3)}
 .wr-v{font-size:15px;font-weight:750}
 .wr-v i{font-size:11px;font-weight:600;color:var(--tx3);font-style:normal;margin-left:2px}
 .wr-t{margin-left:auto;font-size:11px;color:var(--tx3)}
 .wr-sub{font-size:11.5px;color:var(--tx2);margin-top:2px}
 .empty{text-align:center;color:var(--tx3);padding:24px;background:var(--rowbg)}
 #csvbox{width:100%;height:120px;margin-top:8px;font:12px/1.4 ui-monospace,Menlo,monospace;
   background:var(--rowbg);color:var(--tx);border:1px solid var(--brd);border-radius:10px;padding:8px}
 details{margin-top:18px;padding:12px 14px;border-radius:13px;
   background:var(--solid);border:1px solid var(--line)}
 summary{cursor:pointer;color:var(--acc);font-size:13px}
 .foot{color:var(--tx3);font-size:12px;margin-top:8px;line-height:1.7}
 img{width:100%;border-radius:12px;margin-top:10px;background:#000}
 @media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important}}
 @supports not ((backdrop-filter:blur(1px)) or (-webkit-backdrop-filter:blur(1px))){
   .bar,.pill,.last,.whist,.totbox,.tgt{background:var(--solid)}
 }
</style>
<div class=bar>
  <span class=ttl>Balanza · muestreo</span>
  <button class=ib id=thm onclick=themeToggle() aria-label="Cambiar tema"></button>
  <span class=live id=live></span>
</div>
<div class=wrap>

 <div class=screen id=screen>
   <div class=fld>
     <div class=fhead>PESO<span class=u>lb</span>
       <span class=lamp id=lamp><span class=dot></span>ESTABLE</span></div>
     <div class=ledrow id=scPeso></div>
   </div>
   <div class=dv></div>
   <div class=grid2>
     <div class=fld><div class=fhead>PRECIO UNITARIO</div><div class=ledrow id=scPrecio></div></div>
     <div class=fld><div class=fhead>IMPORTE TOTAL</div><div class=ledrow id=scTotal></div></div>
   </div>
 </div>

 <div id=bigbox hidden>
   <div class=lbl style=text-align:center>PESO</div>
   <div class=bigtxt id=bigtxt>--</div>
 </div>

 <div class=prow><span class="pill" id=pill><span class=pd></span><span id=pilltx>...</span></span></div>
 <div class=sub id=sub></div>

 <div class=targets>
   <div class="tgt txt">
     <label for=inSector>Nombre de sector</label>
     <div class=row><input type=text id=inSector maxlength=32 autocomplete=off placeholder="Ej. Sector A" onchange="saveInfo('sector',this)"></div>
   </div>
   <div class="tgt txt">
     <label for=inPisc>Número de piscina</label>
     <div class=row><input type=text id=inPisc maxlength=12 autocomplete=off placeholder="Ej. 12" onchange="saveInfo('piscina',this)"></div>
   </div>
 </div>

 <div class=targets>
   <div class=tgt id=tgtMalla>
     <label for=inMalla>Ingresar peso de la malla</label>
     <div class=row><input type=number id=inMalla inputmode=decimal step=0.01 min=0 placeholder="0.00" onchange="saveTgt('malla',this.value)"><i>lb</i></div>
     <div class=tgtsub id=subMalla>&nbsp;</div>
   </div>
   <div class=tgt id=tgtBin>
     <label for=inBin>Ingresar peso del BIN</label>
     <div class=row><input type=number id=inBin inputmode=decimal step=0.01 min=0 placeholder="0.00" onchange="saveTgt('bin',this.value)"><i>lb</i></div>
     <div class=tgtsub id=subBin>&nbsp;</div>
   </div>
 </div>

 <div class=last id=last></div>

 <div class=totbox>
   <span class=totlbl>PESO TOTAL ACUMULADO</span>
   <span class=totval id=totval>0.00<i>lb</i></span>
   <span class=totn id=totn>0 muestras</span>
 </div>

 <details><summary>Historial</summary>
   <div class=hh>
     <span class=lbl>HISTORIAL</span><span class=cnt id=wc>0</span>
   </div>
   <div class=hact>
     <button id=btnCsv onclick=copyCsv()>Copiar CSV</button>
     <a class=lnk href=/weighings.csv download>Descargar CSV</a>
     <button class=danger id=btnDel onclick=delClick()>Borrar todo</button>
   </div>
   <div class=confirm id=confirm hidden>
     <span>¿Borrar <b>todo</b> el historial?</span>
     <span class=sp></span>
     <button onclick=cancelClear()>Cancelar</button>
     <button class=danger onclick=doClear()>Sí, borrar</button>
   </div>
   <p class=hint>Desliza una fila hacia la izquierda para borrar solo esa.</p>
   <div class=whist id=whist><div class=empty>...</div></div>
   <textarea id=csvbox readonly hidden></textarea>
 </details>
</div>
<div class=gpop id=gpop hidden onclick="hideGpop()">
  <div class=card onclick="event.stopPropagation()">
    <div class=ico><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12l5 5L20 6"/></svg></div>
    <div class=ttl2 id=gpopTtl>Grupo completado</div>
    <div class=val id=gpopVal>0.00<i>lb</i></div>
    <div class=hint2>Toca para cerrar</div>
  </div>
</div>
<script>
function el(id){return document.getElementById(id);}
var SUN='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4.5"/><path d="M12 2v2M12 20v2M4 12H2M22 12h-2M5 5l1.5 1.5M17.5 17.5L19 19M19 5l-1.5 1.5M6.5 17.5L5 19"/></svg>';
var MOON='<svg viewBox="0 0 24 24" fill="currentColor"><path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/></svg>';
function dark(){var t=document.documentElement.dataset.theme;
 return t?t==='dark':matchMedia('(prefers-color-scheme:dark)').matches;}
function paintThm(){el('thm').innerHTML=dark()?SUN:MOON;}
function themeToggle(){var nx=dark()?'light':'dark';
 document.documentElement.dataset.theme=nx;
 try{localStorage.setItem('bz-theme',nx);}catch(e){}
 el('thm').animate([{transform:'rotate(-90deg)',opacity:.3},{transform:'none',opacity:1}],{duration:280});
 paintThm();}
paintThm();

var SEG=["7,7 12,2 40,2 45,7 40,12 12,12","44,9 49,14 49,38 44,43 39,38 39,14",
 "44,45 49,50 49,74 44,79 39,74 39,50","7,81 12,76 40,76 45,81 40,86 12,86",
 "5,45 10,50 10,74 5,79 0,74 0,50","5,9 10,14 10,38 5,43 0,38 0,14",
 "7,44 12,39 40,39 45,44 40,49 12,49"];
function dgt(m,dp,c){var s='<svg class="d '+c+'" viewBox="0 0 54 88">';
 for(var i=0;i<7;i++)s+='<polygon points="'+SEG[i]+'" class="'+(((m>>i)&1)?'on':'off')+'"/>';
 s+='<circle cx=50 cy=83 r=3 class="'+(dp?'on':'off')+'"/>';return s+'</svg>';}
function ledRow(id,mask,dps,from,len,c){var s='';
 for(var i=0;i<len;i++)s+=dgt(mask[from+i]||0,dps[from+i]||0,c);
 el(id).innerHTML=s;}
function f2(x){return (x==null||isNaN(x))?'--':Number(x).toFixed(2);}
var lastN=-1;
async function tick(){
 try{
  var j=await (await fetch('/api',{cache:'no-store'})).json();
  el('live').classList.add('up');
  if(j.disp){
    el('screen').hidden=false; el('bigbox').hidden=true;
    ledRow('scPeso',  j.disp.mask,j.disp.dp,0,5,'xl');
    ledRow('scPrecio',j.disp.mask,j.disp.dp,5,5,'md');
    ledRow('scTotal', j.disp.mask,j.disp.dp,10,6,'md');
    el('lamp').className='lamp'+(j.sniffer.stable?' on':'');
  }else{
    el('screen').hidden=true; el('bigbox').hidden=false;
    el('bigtxt').textContent=f2(j.combined.value);
  }
  var st={vacia:['wait','Esperando gaveta'],
          estabilizando:['weigh','Pesando…'],
          estable:['stable','Peso estable']}[j.capstate]||['wait',j.capstate];
  el('pill').className='pill '+st[0];
  el('pilltx').textContent=st[1];
  el('sub').textContent=(j.capstate=='estable')?'se guarda al retirar la gaveta':('fuente: '+j.combined.source);
  var L=j.last;
  if(L){
    el('last').style.display='block';
    el('last').innerHTML=
      '<div class=top><span class=big>'+f2(L.peso)+'</span><span class=k>lb</span>'+
      '<span class=chip>#'+L.n+'</span></div>'+
      '<div class=meta>precio '+f2(L.precio)+'  ·  total '+f2(L.total)+
      '  ·  '+(L.age<0?'—':('hace '+L.age+' s'))+'</div>';
    if(L.id!=lastN && lastN>=0){ el('last').classList.remove('new');
      void el('last').offsetWidth; el('last').classList.add('new'); tabla(); }
    lastN=L.id;
  }else{ el('last').style.display='none'; }
  el('wc').textContent=j.wcount;
  el('totval').innerHTML=f2(j.wsum)+'<i>lb</i>';
  el('totn').textContent=j.wcount+(j.wcount==1?' muestra':' muestras');
  syncTgtInput('inSector', j.sector);
  syncTgtInput('inPisc',   j.piscina);
  syncTgtInput('inMalla', j.mTgt);
  syncTgtInput('inBin',   j.bTgt);
  tgtProgress('subMalla', j.mSum, j.mTgt, 'malla '+j.mId);
  tgtProgress('subBin',   j.bSum, j.bTgt, 'BIN '+j.bId);
  groupsClosed(j);
 }catch(e){ el('live').classList.remove('up'); }
}
// pone el objetivo en el server (se guarda en NVS, sobrevive a un reinicio)
function saveTgt(kind,val){
  holdInput(kind=='malla'?'inMalla':'inBin');
  var v=parseFloat(val); if(isNaN(v)||v<0) v=0;
  fetch('/targets?'+kind+'='+encodeURIComponent(v)).catch(function(){});
}
// sector / piscina: igual, se guardan en el server (NVS) y sobreviven a un reinicio
function saveInfo(kind,inp){
  holdInput(inp.id);
  fetch('/info?'+kind+'='+encodeURIComponent(inp.value)).catch(function(){});
}
// tras editar un campo, no lo pisa el refresco automatico durante unos segundos
// (deja tiempo a que el server lo guarde, para que no "rebote" al valor viejo)
var holdUntil={};
function holdInput(id){ holdUntil[id]=Date.now()+2500; }
// refleja el valor que tiene el server en el campo, salvo mientras el operario
// lo esta escribiendo (así funciona igual desde varios telefonos)
function syncTgtInput(id,val){
  if(document.activeElement===el(id)) return;
  if(Date.now()<(holdUntil[id]||0)) return;
  var v=(val==null)?'':String(val);
  if(el(id).value!==v) el(id).value=v;
}
// texto de progreso: SIEMPRE contra el ACUMULADO del grupo en curso, no el peso del momento
function tgtProgress(subId,sum,tgt,label){
  var sub=el(subId);
  if(tgt==null){ sub.textContent='sin objetivo puesto'; return; }
  var falt=tgt-sum; if(falt<0) falt=0;
  sub.textContent=label+': '+f2(sum)+' / '+f2(tgt)+' lb  ·  faltan '+f2(falt)+' lb';
}
// detecta que un grupo se acaba de CERRAR y dispara la alerta.  La malla se
// reinicia a 1 en cada BIN, asi que el nº de malla solo NO sirve: se compara el
// par (BIN, malla) con el de la lectura anterior.
//   BIN subio                      -> se cerro un BIN (y su ultima malla)
//   mismo BIN, malla subio         -> se cerro una malla
// Si cierran las dos a la vez sale UN solo aviso, el del BIN.
var prevG=null;
function pulse(id){ var b=el(id); b.classList.remove('pulse'); void b.offsetWidth; b.classList.add('pulse'); }
function groupsClosed(j){
  var cur={b:j.bId,m:j.mId};
  if(prevG!==null){                                   // 1ª lectura: solo memoriza, no dispara
    var binUp=cur.b>prevG.b;
    var mallaUp=!binUp && cur.b===prevG.b && cur.m>prevG.m;
    if(binUp||mallaUp){
      if(mallaUp||j.mTgt!=null) pulse('tgtMalla');
      if(binUp) pulse('tgtBin');
      if(binUp) showGpop('BIN '+(cur.b-1)+' completado', j.bLast);
      else      showGpop('Malla '+(cur.m-1)+' completada', j.mLast);
      beep(); try{ if(navigator.vibrate) navigator.vibrate([120,60,120,60,160]); }catch(e){}
    }
  }
  prevG=cur;
}
var _gpopT=null;
function showGpop(title,total){
  el('gpopTtl').textContent=title;
  el('gpopVal').innerHTML=f2(total)+'<i>lb</i>';
  var g=el('gpop'); g.hidden=false;
  requestAnimationFrame(function(){ g.classList.add('show'); });
  clearTimeout(_gpopT);
  _gpopT=setTimeout(hideGpop,4500);
}
function hideGpop(){
  var g=el('gpop'); g.classList.remove('show');
  clearTimeout(_gpopT);
  setTimeout(function(){ g.hidden=true; },260);
}
var _actx=null;
function beep(){
  try{
    if(!_actx) _actx=new (window.AudioContext||window.webkitAudioContext)();
    var o=_actx.createOscillator(), g=_actx.createGain();
    o.type='sine'; o.frequency.value=880;
    g.gain.setValueAtTime(0.0001,_actx.currentTime);
    g.gain.exponentialRampToValueAtTime(0.35,_actx.currentTime+0.01);
    g.gain.exponentialRampToValueAtTime(0.0001,_actx.currentTime+0.32);
    o.connect(g); g.connect(_actx.destination);
    o.start(); o.stop(_actx.currentTime+0.35);
  }catch(e){}
}
document.addEventListener('pointerdown',function(){ try{ if(!_actx) _actx=new (window.AudioContext||window.webkitAudioContext)(); }catch(e){} },{once:true});
function esc(s){return (''+s).replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function hhmm(t){return (t&&t.length>=19)?t.slice(11,19):'—';}
function ghdr(text,total,nested){
  return '<div class="ghdr'+(nested?' ghdr-m':'')+'">'+esc(text)+'<span class=ghdr-v>'+f2(total)+' lb</span></div>';
}
async function tabla(){
 if(document.querySelector('.wrow.open')) return;   // no refrescar con una fila abierta
 try{
  var j=await (await fetch('/weighings',{cache:'no-store'})).json();
  var box=el('whist');
  if(!j.items||!j.items.length){ box.innerHTML='<div class=empty>sin pesajes todavía</div>'; return; }
  // subtotal por grupo (BIN / malla), sobre lo que se llego a traer aqui
  var binSub={}, mallaSub={};
  for(var i=0;i<j.items.length;i++){var w=j.items[i], v=(w.v==null||isNaN(w.v))?0:w.v;
    binSub[w.b]=(binSub[w.b]||0)+v;
    var mk=w.b+'-'+w.m; mallaSub[mk]=(mallaSub[mk]||0)+v;   // la malla se reinicia en cada BIN -> clave (bin,malla)
  }
  var h='', curB=null, curM=null;
  for(var i=0;i<j.items.length;i++){var w=j.items[i];
    if(w.b!==curB){ h+=ghdr('BIN '+w.b, binSub[w.b]); curB=w.b; curM=null; }
    if(w.m!==curM){ h+=ghdr('Malla '+w.m, mallaSub[w.b+'-'+w.m], true); curM=w.m; }
    var age=(w.age<0)?'':(' · hace '+w.age+' s');
    h+='<div class=wrow data-id="'+w.id+'">'+
         '<button class=wr-del type=button>Borrar</button>'+
         '<div class=wr-front>'+
           '<div class=wr-top><span class=wr-n>#'+w.n+'</span>'+
             '<span class=wr-v>'+f2(w.v)+'<i>lb</i></span>'+
             '<span class=wr-t>'+esc(hhmm(w.t))+'</span></div>'+
           '<div class=wr-sub>precio '+f2(w.pu)+' · total '+f2(w.tot)+age+'</div>'+
         '</div>'+
       '</div>';
  }
  box.innerHTML=h;
  [].forEach.call(box.querySelectorAll('.wrow'),bindRow);
 }catch(e){}
}
function closeRows(except){
  [].forEach.call(document.querySelectorAll('.wrow.open'),function(r){ if(r!==except) r.classList.remove('open'); });
}
function bindRow(row){
  var front=row.querySelector('.wr-front'), sx=0,sy=0,dx=0,drag=false,armed=false,pid=0,W=108;
  row.querySelector('.wr-del').addEventListener('click',function(){ delRow(row.dataset.id); });
  front.addEventListener('pointerdown',function(e){
    sx=e.clientX; sy=e.clientY; dx=0; armed=true; drag=false; pid=e.pointerId;
    front.style.transition='none';
  });
  front.addEventListener('pointermove',function(e){
    if(!armed) return;
    var mx=e.clientX-sx, my=e.clientY-sy;
    if(!drag){
      if(Math.abs(my)>10 && Math.abs(my)>=Math.abs(mx)){ armed=false; front.style.transition=''; return; }
      if(Math.abs(mx)<8) return;
      drag=true; row.classList.add('drag');
      try{ front.setPointerCapture(pid); }catch(_){}
    }
    dx=mx;
    var base=row.classList.contains('open')?-W:0;
    front.style.transform='translateX('+Math.max(-W-24,Math.min(18,base+dx))+'px)';
    e.preventDefault();
  });
  function end(){
    armed=false;
    front.style.transition='';
    if(!drag){ row.classList.remove('open'); return; }   // un toque en fila abierta -> cerrar
    drag=false; row.classList.remove('drag'); front.style.transform='';
    var open=row.classList.contains('open');
    var next = open ? (dx<38) : (dx<-38);
    if(next) closeRows(row);
    row.classList.toggle('open',next);
  }
  front.addEventListener('pointerup',end);
  front.addEventListener('pointercancel',end);
}
function delRow(id){
  fetch('/weighings/del?id='+encodeURIComponent(id)).then(function(){
    lastN=-1; closeRows(null); tabla(); tick();
  });
}
async function copyCsv(){
  var b=el('btnCsv'); b.textContent='…';
  var t='';
  try{ t=await (await fetch('/weighings.csv',{cache:'no-store'})).text(); }
  catch(e){ b.textContent='error'; setTimeout(function(){b.textContent='Copiar CSV';},2000); return; }
  var ok=false;
  try{ if(navigator.clipboard&&navigator.clipboard.writeText){ await navigator.clipboard.writeText(t); ok=true; } }catch(e){}
  if(!ok){
    var ta=el('csvbox'); ta.value=t; ta.hidden=false;
    ta.focus(); ta.select(); try{ta.setSelectionRange(0,t.length);}catch(e){}
    try{ ok=document.execCommand('copy'); }catch(e){}
  }
  if(ok){ el('csvbox').hidden=true; b.textContent='✓ copiado'; }
  else  { b.textContent='selecciónalo abajo ↓'; }
  setTimeout(function(){ b.textContent='Copiar CSV'; },2800);
}
function delClick(){ el('confirm').hidden=false; }
function cancelClear(){ el('confirm').hidden=true; }
function doClear(){
  el('confirm').hidden=true;
  fetch('/weighings/clear').then(function(){ lastN=-1; el('last').style.display='none'; tabla(); tick(); });
}
try{fetch('/settime?epoch='+Math.floor(Date.now()/1000));}catch(e){}
setInterval(tick,1000); tick();
setInterval(tabla,2000); tabla();
</script>
</html>)HTML";

// --------------------------------------------------------------------------
static void handleRoot() {
  S->sendHeader("Cache-Control", "no-store");
  S->send_P(200, "text/html", PAGE_INDEX);
}

static void handleApi() {
  OcrResult o; ocrGetLast(o);
  SnifState s; snifGet(s);
  const bool ocrEn  = ENABLE_OCR;
  const bool snifEn = ENABLE_SNIFFER;
  float    latched = weighLatched();
  int      wc = weighCount();          // pesajes NO borrados

  float comb = NAN; const char* src = "sin dato";
  if      (!isnan(latched))                        { comb = latched;   src = "ultimo pesaje"; }
  else if (snifEn && s.valid && !isnan(s.pesoVal)) { comb = s.pesoVal; src = "bus (en vivo)"; }
  else if (o.valid)                                { comb = o.value;   src = "camara (en vivo)"; }

  int agree = -1;
  if (o.valid && s.valid && !isnan(s.pesoVal))
    agree = (fabsf(o.value - s.pesoVal) <= 0.05f) ? 1 : 0;

  String h; h.reserve(1600);
  h += "{\"ocr\":{\"enabled\":"; h += ocrEn ? "true" : "false";
  h += ",\"valid\":"; h += o.valid ? "true" : "false";
  h += ",\"value\":"; h += o.valid ? String(o.value, 2) : String("null");
  h += ",\"raw\":\""; h += o.text; h += "\",\"ncell\":"; h += (int)o.ncell;
  h += ",\"thr\":"; h += o.thr; h += ",\"autothr\":"; h += o.autoThr;
  h += ",\"bmin\":"; h += o.bMin; h += ",\"bmax\":"; h += o.bMax;
  h += ",\"wstate\":\""; h += weighStateName(); h += "\",\"tplmask\":"; h += ocrTemplatesMask();
  h += "},\"sniffer\":{\"enabled\":"; h += snifEn ? "true" : "false";
  h += ",\"valid\":"; h += s.valid ? "true" : "false";
  h += ",\"stable\":"; h += s.stable ? "true" : "false";
  h += ",\"peso\":\""; h += s.peso; h += "\",\"precio\":\""; h += s.precio;
  h += "\",\"total\":\""; h += s.total; h += "\",\"tx\":"; h += s.txCount;
  h += "}";
#if ENABLE_SNIFFER
  h += ",\"disp\":{"; appendDisp(h, s); h += "}";
#endif
  h += ",\"last\":";
  { Weighing lw;
    if (weighLatchedFull(&lw)) {
      char ts[24]; isoLocal(lw.epoch, ts, sizeof(ts));
      h += "{\"id\":"; h += lw.id;            // clave estable (para el flash "nuevo")
      h += ",\"n\":"; h += wc;                // nº mostrado = posicion (contiguo, sin huecos)
      h += ",\"peso\":"; jnum(h, lw.peso, 2);
      h += ",\"precio\":"; jnum(h, lw.precio, 2);
      h += ",\"total\":"; jnum(h, lw.total, 2);
      h += ",\"age\":"; h += (lw.ms ? String((millis() - lw.ms) / 1000) : String("-1"));
      h += ",\"t\":\""; h += ts; h += "\"}";
    } else h += "null";
  }
  h += ",\"latched\":"; h += isnan(latched) ? String("null") : String(latched, 2);
  h += ",\"wcount\":"; h += wc;
  h += ",\"wsum\":"; h += String(weighSum(), 2);
  { WeighGroups g; weighGetGroups(&g);      // agrupacion MALLA/BIN (acumulado, no en vivo)
    h += ",\"mId\":";   h += g.mallaId;
    h += ",\"mSum\":";  h += String(g.mallaSum, 2);
    h += ",\"mTgt\":";  h += (g.mallaTarget > 0 ? String(g.mallaTarget, 2) : String("null"));
    h += ",\"mLast\":"; jnum(h, g.mallaLast, 2);
    h += ",\"bId\":";   h += g.binId;
    h += ",\"bSum\":";  h += String(g.binSum, 2);
    h += ",\"bTgt\":";  h += (g.binTarget > 0 ? String(g.binTarget, 2) : String("null"));
    h += ",\"bLast\":"; jnum(h, g.binLast, 2);
  }
  h += ",\"combined\":{\"value\":"; h += isnan(comb) ? String("null") : String(comb, 2);
  h += ",\"source\":\""; h += src; h += "\"}";
  h += ",\"sector\":";  jstr(h, g_sector);
  h += ",\"piscina\":"; jstr(h, g_piscina);
  h += ",\"agree\":"; h += (agree < 0 ? "null" : (agree ? "true" : "false"));
  h += ",\"capstate\":\""; h += weighStateName(); h += "\"";
  h += ",\"rssi\":"; h += (int)WiFi.RSSI();
  h += ",\"uptime\":"; h += (unsigned long)(millis() / 1000);
  h += "}";

  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "application/json", h);
}

// --------------------------------------------------------------------------
//  Historial de pesajes  (disponible con cualquier metodo activo)
static void handleWeighings() {
  const int SHOW = 30;
  Weighing w[SHOW];
  int n = weighGet(w, SHOW);
  float latched = weighLatched();
  uint32_t now = millis();

  String h; h.reserve(3200);
  h += "{\"latched\":";
  h += isnan(latched) ? "null" : String(latched, 2);
  int alive = weighCount();
  h += ",\"count\":"; h += alive; h += ",\"items\":[";
  for (int i = 0; i < n; i++) {
    char ts[24]; isoLocal(w[i].epoch, ts, sizeof(ts));
    if (i) h += ',';
    h += "{\"id\":"; h += w[i].id;            // clave estable para borrar
    h += ",\"n\":"; h += (alive - i);         // nº mostrado = posicion (contiguo)
    h += ",\"v\":";  jnum(h, w[i].peso, 2);
    h += ",\"pu\":"; jnum(h, w[i].precio, 2);
    h += ",\"tot\":"; jnum(h, w[i].total, 2);
    h += ",\"m\":"; h += w[i].malla;          // nº de MALLA a la que pertenece
    h += ",\"b\":"; h += w[i].bin;            // nº de BIN   a la que pertenece
    h += ",\"age\":"; h += (w[i].ms ? String((now - w[i].ms) / 1000) : String("-1"));
    h += ",\"t\":\""; h += ts; h += "\"}";
  }
  h += "]}";
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "application/json", h);
}

static void handleWeighingsCsv() {
  Weighing* w = (Weighing*)malloc(sizeof(Weighing) * WEIGH_LOG_SIZE);
  if (!w) { S->send(500, "text/plain", "sin memoria"); return; }
  int n = weighGet(w, WEIGH_LOG_SIZE);            // reciente primero

  S->sendHeader("Content-Disposition", "attachment; filename=pesajes.csv");
  S->sendHeader("Cache-Control", "no-store");
  S->setContentLength(CONTENT_LENGTH_UNKNOWN);
  S->send(200, "text/csv", "");
  S->sendContent("n;peso;precio_unit;total;malla;bin;hora_local;sector;piscina\n");
  String row; row.reserve(256);
  for (int i = n - 1; i >= 0; i--) {                 // del mas viejo (nº 1) al mas nuevo (nº n)
    char ts[24]; isoLocal(w[i].epoch, ts, sizeof(ts));
    row  = String(n - i);  row += ';';                 // nº = posicion (contiguo)
    row += String(w[i].peso, 2);   row += ';';
    row += (isnan(w[i].precio) ? String("") : String(w[i].precio, 2)); row += ';';
    row += (isnan(w[i].total)  ? String("") : String(w[i].total, 2));  row += ';';
    row += String(w[i].malla); row += ';';
    row += String(w[i].bin);   row += ';';
    row += ts; row += ';';
    row += g_sector;  row += ';';                      // de la jornada: valor actual al exportar
    row += g_piscina; row += '\n';
    S->sendContent(row);
  }
  S->sendContent("");
  free(w);
}

static void handleWeighingsClear() {
  weighClear();
  Serial.println("[web] historial borrado");
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", "ok");
}

//  borra UNA fila:  /weighings/del?id=12
static void handleWeighingsDelOne() {
  uint32_t id = S->hasArg("id") ? (uint32_t)S->arg("id").toInt() : 0;
  bool ok = (id > 0) && weighDeleteOne(id);
  S->sendHeader("Cache-Control", "no-store");
  S->send(ok ? 200 : 404, "text/plain", ok ? "ok" : "no existe");
}

//  el operario pone los objetivos de MALLA/BIN (en lb):  /targets?malla=200&bin=800
//  (0 desactiva ese objetivo; el parametro AUSENTE deja ese objetivo como estaba,
//   ya que el panel manda solo el campo que el operario cambio)
static void handleSetTargets() {
  WeighGroups g; weighGetGroups(&g);
  float mt = S->hasArg("malla") ? S->arg("malla").toFloat() : g.mallaTarget;
  float bt = S->hasArg("bin")   ? S->arg("bin").toFloat()   : g.binTarget;
  weighSetTargets(mt, bt);
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", "ok");
}

//  el operario pone nombre de sector y nº de piscina:  /info?sector=A&piscina=12
//  (vacio los borra; el parametro AUSENTE deja ese dato como estaba, porque el
//   panel manda solo el campo que el operario cambio)
static void handleSetInfo() {
  bool changed = false;
  if (S->hasArg("sector")) {
    String v = cleanInfo(S->arg("sector"), SECTOR_MAX_BYTES);
    if (v != g_sector) { g_sector = v; changed = true; }
  }
  if (S->hasArg("piscina")) {
    String v = cleanInfo(S->arg("piscina"), PISCINA_MAX_BYTES);
    if (v != g_piscina) { g_piscina = v; changed = true; }
  }
  if (changed) infoSave();
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", "ok");
}

//  el movil del operario pone la hora al abrir el panel (util sin internet)
static void handleSetTime() {
  long ep = S->hasArg("epoch") ? S->arg("epoch").toInt() : 0;
  if (ep <= 1700000000) { S->send(400, "text/plain", "epoch invalido"); return; }
  struct timeval tv; tv.tv_sec = (time_t)ep; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  S->send(200, "text/plain", "ok");
}

#if ENABLE_OCR
static void sendJpeg(bool debug) {
  uint8_t* buf = nullptr; size_t len = 0;
  bool ok = debug ? ocrJpegDebug(&buf, &len) : ocrJpegSnapshot(&buf, &len);
  if (!ok || !buf) { S->send(503, "text/plain", "sin imagen"); return; }
  S->sendHeader("Cache-Control", "no-store");
  S->setContentLength(len);
  S->send(200, "image/jpeg", "");
  S->sendContent((const char*)buf, len);
  free(buf);
}
#endif  // ENABLE_OCR

// --------------------------------------------------------------------------
#if ENABLE_SNIFFER
// captura un estado conocido:  /snif_cap?p=6.50&pr=1.00&t=6.50
static void handleSnifCap() {
  int n = snifCapture(S->arg("p").c_str(), S->arg("pr").c_str(), S->arg("t").c_str());
  char m[48];
  if (n < 0) snprintf(m, sizeof(m), "buffer lleno");
  else       snprintf(m, sizeof(m), "capturas: %d", n);
  S->send(200, "text/plain", m);
}
static void handleSnifCapClear() { snifCapClear(); S->send(200, "text/plain", "capturas borradas"); }
static void handleSnifSolve() {
  char rep[160];
  snifSolve(rep, sizeof(rep));
  S->send(200, "text/plain", rep);
}

static void handleSniffer() {
  String h; h.reserve(9200);
  h += F("<!doctype html><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>bus TM1640</title><style>"
         "body{font-family:ui-monospace,Consolas,monospace;background:#111;color:#ddd;padding:12px}"
         "table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid #444;padding:2px 8px;text-align:center}"
         "a{color:#64b5f6}.on{color:#7fdb7f}input{background:#222;color:#eee;border:1px solid #555;width:80px}"
         "button{background:#2a2a2a;color:#eee;border:1px solid #666;padding:4px 10px;cursor:pointer}"
         "fieldset{border:1px solid #444;margin:10px 0}"
         ".bad{color:#ff6b6b}.ok{color:#7fdb7f}"
         ".screen{background:#0b0b0b;border:1px solid #2c2c2c;border-radius:12px;padding:12px 14px;max-width:560px}"
         ".fld{margin-top:8px}.fld:first-child{margin-top:0}"
         ".fhead{font-size:10px;letter-spacing:.14em;color:#e5533b;margin-bottom:2px}"
         ".ledrow{display:flex;align-items:flex-end;gap:1px;overflow-x:auto}"
         ".d{flex:0 0 auto}.d.xl{height:66px}.d.md{height:40px}"
         ".d .on{fill:#ff3a22}.d .off{fill:#ff3a22;opacity:.09}"
         ".two{display:flex;gap:22px;flex-wrap:wrap}"
         ".estable{color:#39d98a;font-size:11px;letter-spacing:.1em}</style>");

  h += F("<div class=screen>"
         "<div class=fld><div class=fhead>PESO lb &nbsp;<span id=estable class=estable></span></div>"
         "<div class=ledrow id=scPeso></div></div>"
         "<div class=two>"
         "<div class=fld><div class=fhead>PRECIO UNITARIO</div><div class=ledrow id=scPrecio></div></div>"
         "<div class=fld><div class=fhead>IMPORTE TOTAL</div><div class=ledrow id=scTotal></div></div>"
         "</div></div>"
         "<div id=rd style=font-size:15px;margin:8px 0></div>"
         "<div id=st style=color:#999;font-size:13px></div>"
         "<div id=mapln style=color:#999;font-size:12px;margin-top:4px></div>"
         "<button onclick=mapreset() style=margin-top:4px>Restaurar mapa por defecto</button>"
         "<span id=mapmsg style=color:#7fdb7f;margin-left:8px></span>");

  h += F("<h3>0. Diagnostico de cableado</h3>"
         "<fieldset><legend>flancos vistos en cada pin (deben SUBIR)</legend>"
         "<div id=diag style=font-size:15px>...</div>"
         "<p style=color:#999>SL y DA en reposo estan en ALTO (nivel=1). Los "
         "'flancos' cuentan cualquier transicion; si NO suben con la balanza "
         "encendida y el display cambiando, el cable no llega (revisa masa comun, "
         "pad correcto, soldadura).</p>"
         "<button onclick=probe()>Probar pines (pull-up/down)</button> "
         "<button onclick=rst()>Reiniciar medida SL</button>"
         "<pre id=probemsg style=color:#ffd479;margin:6px 0;white-space:pre-wrap></pre>"
         "</fieldset>");

  h += F("<h3>1. Resolver el mapa automaticamente</h3>"
         "<fieldset><legend>capturar un estado conocido</legend>"
         "<p>Enciende la balanza. Pon un peso, MIRA lo que marca y escribelo. Repite con "
         "3-4 pesos MUY distintos (e incluye uno pesado de 4-5 cifras). Luego 'Resolver'.</p>"
         "PESO <input id=cp placeholder=6.50> &nbsp;"
         "PRECIO <input id=cpr placeholder=(opc)> &nbsp;"
         "TOTAL <input id=ct placeholder=(opc)> &nbsp;"
         "<button onclick=cap()>Capturar</button>"
         "<div id=capmsg style=color:#7fdb7f;margin-top:6px>capturas: 0</div>"
         "<button onclick=solve()>Resolver mapa</button> "
         "<button onclick=capclr()>Borrar capturas</button>"
         "<div id=solvemsg style=color:#ffd479;margin-top:6px></div>"
         "</fieldset>");

  h += F("<details><summary>2. RAM del display (16 bytes, detalle tecnico)</summary>"
         "<table id=ram><tr><th>addr<th>hex<th>bin 7..0<th>7seg*</tr></table>"
         "<p>* con el mapa actual. <a href=/config>editar a mano</a> &nbsp;|&nbsp; "
         "<a href=/sniffer/raw>volcado crudo</a></p></details>"
         "<p><a href=/>&larr; volver al panel</a></p>");

  h += "<script>var SLP="; h += (int)SNIF_PIN_SL; h += ",DAP="; h += (int)SNIF_PIN_DA; h += ";";
  h += F(
   "var SEG=[\"6,6 10,2 38,2 42,6 38,10 10,10\",\"42,8 38,12 38,37 42,41 46,37 46,12\","
   "\"42,45 38,49 38,74 42,78 46,74 46,49\",\"6,79 10,75 38,75 42,79 38,83 10,83\","
   "\"6,45 2,49 2,74 6,78 10,74 10,49\",\"6,8 2,12 2,37 6,41 10,37 10,12\","
   "\"6,43 10,39 38,39 42,43 38,47 10,47\"];"
   "function dgt(m,dp,c){var s='<svg class=\"d '+c+'\" viewBox=\"0 0 52 86\">';"
   "for(var i=0;i<7;i++)s+='<polygon points=\"'+SEG[i]+'\" class=\"'+(((m>>i)&1)?'on':'off')+'\"/>';"
   "s+='<circle cx=48 cy=80 r=3 class=\"'+(dp?'on':'off')+'\"/>';return s+'</svg>';}"
   "function ledRow(id,mk,dp,fr,ln,c){var s='';for(var i=0;i<ln;i++)s+=dgt(mk[fr+i]||0,dp[fr+i]||0,c);"
   "document.getElementById(id).innerHTML=s;}");
  h += F(
   "function ram(){fetch('/snif.json',{cache:'no-store'}).then(function(x){return x.json();}).then(function(j){"
   "ledRow('scPeso',j.mask,j.dp,0,5,'xl');ledRow('scPrecio',j.mask,j.dp,5,5,'md');ledRow('scTotal',j.mask,j.dp,10,6,'md');"
   "document.getElementById('estable').textContent=j.stable?'\\u25cf ESTABLE':'';"
   "var t='<tr><th>addr<th>hex<th>bin 7..0<th>7seg*</tr>';"
   "for(var i=0;i<16;i++){var v=j.ram[i],b='';for(var k=7;k>=0;k--)b+=((v>>k)&1);"
   "t+='<tr><td>D'+(i<10?'0':'')+i+'<td>0x'+(v<16?'0':'')+v.toString(16)+'<td>'+b+'<td>'+j.seg[i]+'</tr>';}"
   "document.getElementById('ram').innerHTML=t;"
   "function nn(x){return x==null?'NaN':x;}"
   "document.getElementById('rd').innerHTML='valor decodificado &nbsp; PESO <b>'+nn(j.pv)+'</b>  PRECIO <b>'+nn(j.prv)+'</b>  TOTAL <b>'+nn(j.tv)+'</b>'"
   "+(j.pv==null?'  <span style=color:#ff6b6b>(PESO no decodifica -> el pesaje NO se guarda)</span>':'');"
   "document.getElementById('st').textContent=j.tx+' tramas · '+j.refr+' refrescos · SL '+j.slkhz+' kHz media / '+j.slpk+' kHz pico ('+j.slfast+' pulsos rapidos)';"
   "document.getElementById('mapln').textContent='mapa  PESO['+j.mpeso+']  PRECIO['+j.mprecio+']  TOTAL['+j.mtotal+']';"
   "var ds=(window.PSE==null)?0:(j.sle-window.PSE), dd=(window.PDE==null)?0:(j.dae-window.PDE);"
   "window.PSE=j.sle; window.PDE=j.dae;"
   "function pin(nm,gp,lv,tot,dl){var c=dl>0?'ok':'bad';var s=dl>0?('+'+dl+'/s'):'PARADO';"
   "return '<span class=\"'+c+'\">'+nm+' GPIO'+gp+'  nivel='+lv+'  flancos='+tot+'  ('+s+')</span>';}"
   "document.getElementById('diag').innerHTML=pin('SL',SLP,j.sl,j.sle,ds)+'<br>'+pin('DA',DAP,j.da,j.dae,dd);"
   "}).catch(function(){});}"
   "function probe(){document.getElementById('probemsg').textContent='probando...';"
   "fetch('/snif_probe').then(function(x){return x.text();}).then(function(t){document.getElementById('probemsg').textContent=t;});}"
   "function rst(){window.PSE=null;window.PDE=null;fetch('/snif_reset').then(function(x){return x.text();}).then(function(t){document.getElementById('probemsg').textContent=t;});}"
   "function mapreset(){fetch('/snif_mapreset').then(function(x){return x.text();}).then(function(t){document.getElementById('mapmsg').textContent=t;ram();});}"
   "function q(){return 'p='+encodeURIComponent(cp.value)+'&pr='+encodeURIComponent(cpr.value)+'&t='+encodeURIComponent(ct.value);}"
   "function cap(){fetch('/snif_cap?'+q()).then(function(x){return x.text();}).then(function(t){document.getElementById('capmsg').textContent=t;});}"
   "function capclr(){fetch('/snif_capclear').then(function(x){return x.text();}).then(function(t){document.getElementById('capmsg').textContent=t;});}"
   "function solve(){document.getElementById('solvemsg').textContent='resolviendo...';"
   "fetch('/snif_solve').then(function(x){return x.text();}).then(function(t){document.getElementById('solvemsg').textContent=t;ram();});}"
   "setInterval(ram,1000);ram();"
   "</script>");
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/html", h);
}

// datos del sniffer en JSON, para refrescar /sniffer sin recargar
static void handleSnifJson() {
  SnifState s; snifGet(s);
  String h; h.reserve(1100);
  h += "{\"ram\":[";
  for (int i = 0; i < 16; i++) { if (i) h += ','; h += (int)s.shadow[i]; }
  h += "],\"seg\":[";
  for (int i = 0; i < 16; i++) {
    int d = seg7decode(s.shadow[i]);
    if (i) h += ',';
    h += '"'; h += (d >= 0) ? String(d) : (d == -2 ? String("_") : String("?")); h += '"';
  }
  h += "],";
  appendDisp(h, s);                       // "mask":[...] EN ORDEN DE LECTURA (mapa aplicado)
  h += ",\"peso\":\""; h += s.peso;
  h += "\",\"precio\":\""; h += s.precio;
  h += "\",\"total\":\""; h += s.total;
  h += "\",\"pv\":";  jnum(h, s.pesoVal, 2);
  h += ",\"prv\":";   jnum(h, s.precioVal, 2);
  h += ",\"tv\":";    jnum(h, s.totalVal, 2);
  h += ",\"stable\":"; h += s.stable ? "true" : "false";
  { // mapa actual, para verlo/depurarlo
    const int8_t* mp[3] = { snf.pesoAddr, snf.precioAddr, snf.totalAddr };
    int8_t ml[3] = { snf.pesoLen, snf.precioLen, snf.totalLen };
    const char* nm[3] = { "mpeso", "mprecio", "mtotal" };
    for (int f = 0; f < 3; f++) {
      h += ",\""; h += nm[f]; h += "\":\"";
      for (int i = 0; i < ml[f]; i++) { if (i) h += ','; h += (int)mp[f][i]; }
      h += '"';
    }
  }
  h += ",\"tx\":"; h += s.txCount;
  h += ",\"refr\":"; h += s.refreshCount;
  h += ",\"slkhz\":"; h += String(s.slKHz, 0);
  h += ",\"slpk\":"; h += String(s.slKHzPeak, 0);
  h += ",\"slfast\":"; h += s.slFastN;
  { uint8_t sl, da; uint32_t se, de; snifDiag(&sl, &da, &se, &de);
    h += ",\"sl\":";  h += (int)sl;
    h += ",\"da\":";  h += (int)da;
    h += ",\"sle\":"; h += se;
    h += ",\"dae\":"; h += de; }
  h += "}";
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "application/json", h);
}

static void handleSnifProbe() {
  char rep[200];
  snifPinProbe(rep, sizeof(rep));
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", rep);
}

static void handleSnifReset() {
  snifResetStats();
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", "medidas de reloj/flancos reiniciadas");
}

static void handleSnifMapReset() {
  snifResetParams();
  S->sendHeader("Cache-Control", "no-store");
  S->send(200, "text/plain", "mapa restaurado al de config.h");
}

static void handleSnifRaw() {
  snifArmRaw();
  delay(320);
  int n = snifRawCount();
  S->sendHeader("Cache-Control", "no-store");
  S->setContentLength(CONTENT_LENGTH_UNKNOWN);
  S->send(200, "text/plain", "");
  char head[80];
  snprintf(head, sizeof(head), "# n=%d cpuMHz=%lu  formato: <delta_ciclos> <SL> <DA>\n",
           n, (unsigned long)snifCpuMHz());
  S->sendContent(head);
  String chunk; chunk.reserve(2048);
  for (int i = 0; i < n; i++) {
    uint32_t d; uint8_t sl, da;
    snifRawGet(i, &d, &sl, &da);
    chunk += d; chunk += ' '; chunk += sl; chunk += ' '; chunk += da; chunk += '\n';
    if (chunk.length() > 1800) { S->sendContent(chunk); chunk = ""; }
  }
  if (chunk.length()) S->sendContent(chunk);
  S->sendContent("");
}
#endif  // ENABLE_SNIFFER

// --------------------------------------------------------------------------
static int   argI(const char* k, int def)   { return S->hasArg(k) ? S->arg(k).toInt()   : def; }
static float argF(const char* k, float def) { return S->hasArg(k) ? S->arg(k).toFloat() : def; }

static void field(String& h, const char* name, const String& val, const char* label, int width = 90) {
  h += "<label>"; h += label; h += "</label><input name="; h += name;
  h += " style=width:"; h += width; h += "px value='"; h += val; h += "'>";
}

#if ENABLE_OCR
// lee los campos del formulario OCR -> ocr.*  (sin guardar en NVS)
static void applyOcrFormArgs() {
  ocr.autoMode    = argI("auto", ocr.autoMode ? 1 : 0) != 0;
  ocr.digitAspect = argF("asp",  ocr.digitAspect);
  ocr.digitPitch  = argF("pit",  ocr.digitPitch);
  ocr.autoBright  = argI("abr",  ocr.autoBright);
  ocr.boxX = argI("boxX", ocr.boxX);   ocr.boxY = argI("boxY", ocr.boxY);
  ocr.boxW = argI("boxW", ocr.boxW);   ocr.boxH = argI("boxH", ocr.boxH);
  ocr.digitGap    = argI("gap",  ocr.digitGap);
  ocr.slant       = argF("slant", ocr.slant);
  ocr.onRatio     = argF("onR",  ocr.onRatio);
  ocr.minContrast = argI("minC", ocr.minContrast);
  ocr.numDigits   = argI("nd",   ocr.numDigits);
  ocr.decimals    = argI("dec",  ocr.decimals);
  ocr.aecValue    = argI("aec",  ocr.aecValue);
  ocr.contrast    = argI("cont", ocr.contrast);
  ocr.flashLed    = argI("flash", ocr.flashLed ? 1 : 0) != 0;
  if (ocr.numDigits < 1) ocr.numDigits = 1;
  if (ocr.numDigits > 8) ocr.numDigits = 8;
  if (ocr.decimals < 0)  ocr.decimals = 0;
  if (ocr.decimals > 4)  ocr.decimals = 4;
  if (ocr.digitAspect < 0.2f) ocr.digitAspect = 0.2f;
  if (ocr.digitAspect > 1.2f) ocr.digitAspect = 1.2f;
  if (ocr.digitPitch  < 1.0f) ocr.digitPitch  = 1.0f;
  if (ocr.digitPitch  > 3.0f) ocr.digitPitch  = 3.0f;
  if (ocr.boxW < 10) ocr.boxW = 10;
  if (ocr.boxH < 10) ocr.boxH = 10;
}

// vista previa en vivo: aplica los cambios a RAM y responde (no toca NVS)
static void handleOcrLive() {
  int oldAec = ocr.aecValue, oldCont = ocr.contrast;
  applyOcrFormArgs();
  if (ocr.aecValue != oldAec || ocr.contrast != oldCont) ocrApplyCam();
  S->send(200, "text/plain", "ok");
}

// aprender plantillas:  /ocr_learn?d=6.50   o   /ocr_learn?clear=1
static void handleOcrLearn() {
  if (S->hasArg("clear")) { ocrTemplatesClear(); S->send(200, "text/plain", "plantillas borradas"); return; }
  if (!S->hasArg("d") || S->arg("d").length() == 0) {
    S->send(200, "text/plain", "escribe el valor que se ve ahora (ej 6.50)"); return;
  }
  int r = ocrLearn(S->arg("d").c_str());
  char m[72];
  if      (r > 0)  snprintf(m, sizeof(m), "OK: %d cifra(s) aprendida(s)", r);
  else if (r == -1) snprintf(m, sizeof(m), "ERROR: escribiste mas cifras de las celdas que veo");
  else if (r == -2) snprintf(m, sizeof(m), "ERROR: ahora mismo no localizo el display");
  else if (r == -3) snprintf(m, sizeof(m), "ERROR: pon solo el numero (ej 6.50)");
  else              snprintf(m, sizeof(m), "ERROR (%d)", r);
  S->send(200, "text/plain", m);
}
#endif

static void handleConfigGet() {
  String h; h.reserve(9000);
  h += F("<!doctype html><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Calibrar</title><style>"
         "body{font-family:system-ui,Segoe UI,sans-serif;background:#111;color:#eee;padding:12px;max-width:760px;margin:auto}"
         "img{width:100%;border-radius:8px;background:#000}"
         "label{display:inline-block;width:135px;font-size:13px}"
         "input{margin:3px;background:#222;color:#eee;border:1px solid #555;border-radius:4px;padding:4px}"
         "fieldset{border:1px solid #444;border-radius:8px;margin:12px 0}"
         "button{padding:6px 12px;margin:2px;background:#2a2a2a;color:#eee;border:1px solid #666;border-radius:6px;cursor:pointer}"
         "a{color:#64b5f6}legend{color:#aaa}</style>");
  h += "<form method=post>";

#if ENABLE_OCR
  {
    h += F("<h3>Calibracion OCR</h3><img id=dbg src=/ocr_debug.jpg>");
    h += F("<div style=margin:6px 0>"
           "<button type=button onclick=r()>refrescar</button> &nbsp;"
           "<button type=button onclick=\"n('boxX',-5)\">&larr;</button>"
           "<button type=button onclick=\"n('boxX',5)\">&rarr;</button> "
           "<button type=button onclick=\"n('boxY',-5)\">&uarr;</button>"
           "<button type=button onclick=\"n('boxY',5)\">&darr;</button> &nbsp;"
           "<button type=button onclick=\"n('boxW',-5)\">ancho-</button>"
           "<button type=button onclick=\"n('boxW',5)\">ancho+</button> "
           "<button type=button onclick=\"n('boxH',-5)\">alto-</button>"
           "<button type=button onclick=\"n('boxH',5)\">alto+</button></div>");
    h += F("<p>Lectura OCR: <b id=rd>--</b></p>");
    h += F("<fieldset><legend>Localizacion</legend>");
    field(h, "auto", String(ocr.autoMode ? 1 : 0), "auto (1=busca sola)");
    field(h, "nd",   String(ocr.numDigits),        "n digitos (max)");
    field(h, "dec",  String(ocr.decimals),         "decimales");
    field(h, "slant",String(ocr.slant, 3),         "inclinacion");
    h += F("<br><small style=color:#999>auto=1: no hace falta encuadrar. "
           "asp = ancho de digito / alto ; pit = separacion entre digitos.</small><br>");
    field(h, "asp",  String(ocr.digitAspect, 2),   "asp (ancho/alto)");
    field(h, "pit",  String(ocr.digitPitch, 2),    "pit (separacion)");
    h += F("</fieldset><fieldset><legend>Caja manual (solo si auto=0)</legend>");
    field(h, "boxX", String(ocr.boxX), "boxX");
    field(h, "boxY", String(ocr.boxY), "boxY");
    field(h, "boxW", String(ocr.boxW), "boxW");
    field(h, "boxH", String(ocr.boxH), "boxH");
    field(h, "gap",  String(ocr.digitGap), "separacion");
    h += F("</fieldset><fieldset><legend>Deteccion y camara</legend>");
    field(h, "onR",  String(ocr.onRatio, 2),       "umbral seg 0..1");
    field(h, "minC", String(ocr.minContrast),      "contraste min");
    field(h, "abr",  String(ocr.autoBright),       "brillo ON (0=auto)");
    field(h, "aec",  String(ocr.aecValue),         "exposicion");
    field(h, "cont", String(ocr.contrast),         "contraste cam");
    field(h, "flash",String(ocr.flashLed ? 1 : 0), "flash LED");
    h += F("</fieldset>"
           "<fieldset><legend>Plantillas (reconocimiento por forma)</legend>"
           "<div id=tplinfo style=font-size:13px;color:#999>cargando...</div>"
           "<p style=font-size:13px;color:#999>Pon un peso, mira que numero marca, "
           "escribelo aqui y pulsa Aprender.  Repite con varios pesos hasta cubrir "
           "las 10 cifras (0-9).  Ej: 6.50 -> aprende 6, 5 y 0.</p>"
           "<input id=tplshown placeholder='ej 6.50' style=width:120px>"
           "<button type=button onclick=aprender()>Aprender</button> "
           "<button type=button onclick=olvidar()>Olvidar todas</button>"
           "<div id=tplmsg style=font-size:13px;color:#7fdb7f;margin-top:4px></div>"
           "</fieldset>");
  }
#endif

#if ENABLE_SNIFFER
  {
    String segcsv, pcsv, prcsv, tcsv;
    for (int i = 0; i < 8; i++)             { if (i) segcsv += ','; segcsv += (int)snf.segBit[i]; }
    for (int i = 0; i < snf.pesoLen; i++)   { if (i) pcsv   += ','; pcsv   += (int)snf.pesoAddr[i]; }
    for (int i = 0; i < snf.precioLen; i++) { if (i) prcsv  += ','; prcsv  += (int)snf.precioAddr[i]; }
    for (int i = 0; i < snf.totalLen; i++)  { if (i) tcsv   += ','; tcsv   += (int)snf.totalAddr[i]; }
    h += F("<h3>Mapa del sniffer</h3><fieldset><legend>Indices D00..D15 (izq -> der)</legend>");
    field(h, "peso",   pcsv,   "PESO addr",   170);
    field(h, "precio", prcsv,  "PRECIO addr", 170);
    field(h, "total",  tcsv,   "TOTAL addr",  170);
    h += "<br>";
    field(h, "segbit", segcsv, "segBit a..g,dp", 170);
    field(h, "pdec",   String(snf.pesoDP),   "PESO dec");
    field(h, "prdec",  String(snf.precioDP), "PRECIO dec");
    field(h, "tdec",   String(snf.totalDP),  "TOTAL dec");
    h += F("</fieldset>");
  }
#endif

  h += F("<button type=submit>Guardar (para que sobreviva al reinicio)</button> "
         "&nbsp; <a href=/>volver</a></form>");
  h += F("<script>function r(){var d=document.getElementById('dbg');"
         "if(d)d.src='/ocr_debug.jpg?'+Date.now();}");
#if ENABLE_OCR
  h += F("function live(){var q=new URLSearchParams(new FormData(document.forms[0])).toString();"
         "fetch('/ocr_live?'+q).then(r).catch(function(){});}"
         "function n(k,d){var e=document.getElementsByName(k)[0];"
         "e.value=(parseInt(e.value)||0)+d;live();}"
         "document.querySelectorAll('form input').forEach(function(el){el.addEventListener('change',live);});"
         "async function poll(){try{var j=await (await fetch('/api',{cache:'no-store'})).json();"
         "var t=j.ocr.valid?('VALOR '+Number(j.ocr.value).toFixed(2)):('cruda \"'+(j.ocr.raw||'')+'\"');"
         "document.getElementById('rd').textContent=t+'   -   '+j.ocr.ncell+' celdas, "
         "umbral seg '+j.ocr.thr+' / auto '+j.ocr.autothr+', brillo '+j.ocr.bmin+'-'+j.ocr.bmax;"
         "}catch(e){}}"
         "function tplRefresh(){fetch('/api',{cache:'no-store'}).then(function(x){return x.json();}).then(function(j){"
         "var m=j.ocr.tplmask||0,hay=[],falta=[];for(var d=0;d<10;d++){(m&(1<<d)?hay:falta).push(d);}"
         "document.getElementById('tplinfo').textContent="
         "(m==0?'sin plantillas -> se leen por 7 segmentos':"
         "('aprendidas: '+hay.join(' ')+(falta.length?('    faltan: '+falta.join(' ')):'    (todas! listo)')));"
         "}).catch(function(){});}"
         "function aprender(){var v=document.getElementById('tplshown').value;"
         "fetch('/ocr_learn?d='+encodeURIComponent(v)).then(function(x){return x.text();}).then(function(t){"
         "document.getElementById('tplmsg').textContent=t;tplRefresh();r();});}"
         "function olvidar(){if(confirm('Borrar TODAS las plantillas?'))"
         "fetch('/ocr_learn?clear=1').then(function(x){return x.text();}).then(function(t){"
         "document.getElementById('tplmsg').textContent=t;tplRefresh();});}"
         "setInterval(function(){r();poll();tplRefresh();},2000);r();poll();tplRefresh();");
#else
  h += F("function n(){}setInterval(r,2000);");
#endif
  h += F("</script>");
  S->send(200, "text/html", h);
}

static void handleConfigPost() {
#if ENABLE_OCR
  applyOcrFormArgs();
  ocrSaveParams();            // aplica al sensor + guarda en NVS
#endif
#if ENABLE_SNIFFER
  {
    Preferences pr;
    pr.begin("snf", false);
    // solo se guarda si el campo trae algo (evita romper el mapa con un blanco)
    if (S->arg("peso").length()   >= 3) pr.putString("peso",   S->arg("peso"));
    if (S->arg("precio").length() >= 3) pr.putString("precio", S->arg("precio"));
    if (S->arg("total").length()  >= 3) pr.putString("total",  S->arg("total"));
    if (S->arg("segbit").length() >= 3) pr.putString("segbit", S->arg("segbit"));
    pr.putInt("pdec",  argI("pdec",  snf.pesoDP));
    pr.putInt("prdec", argI("prdec", snf.precioDP));
    pr.putInt("tdec",  argI("tdec",  snf.totalDP));
    pr.end();
    snifLoadParams();
  }
#endif
  S->sendHeader("Location", "/config");
  S->send(303, "text/plain", "guardado");
}

// --------------------------------------------------------------------------
void webBegin(WebServer& srv) {
  S = &srv;
  infoLoad();                                          // sector / piscina guardados
  S->on("/",    handleRoot);
  S->on("/api", handleApi);
  S->on("/weighings",       handleWeighings);
  S->on("/weighings.csv",   handleWeighingsCsv);
  S->on("/weighings/clear", handleWeighingsClear);   // GET o POST, da igual
  S->on("/weighings/del",   handleWeighingsDelOne);   // ?id=12 -> borra esa fila
  S->on("/targets",         handleSetTargets);        // ?malla=200&bin=800
  S->on("/info",            handleSetInfo);           // ?sector=A&piscina=12
  S->on("/settime",         handleSetTime);
#if ENABLE_OCR
  S->on("/ocr_live",        handleOcrLive);
  S->on("/ocr_learn",       handleOcrLearn);
  S->on("/snapshot.jpg",    []() { sendJpeg(false); });
  S->on("/ocr_debug.jpg",   []() { sendJpeg(true);  });
#endif
#if ENABLE_SNIFFER
  S->on("/sniffer",        handleSniffer);
  S->on("/snif.json",      handleSnifJson);
  S->on("/sniffer/raw",    handleSnifRaw);
  S->on("/snif_cap",       handleSnifCap);
  S->on("/snif_capclear",  handleSnifCapClear);
  S->on("/snif_solve",     handleSnifSolve);
  S->on("/snif_probe",     handleSnifProbe);
  S->on("/snif_reset",     handleSnifReset);
  S->on("/snif_mapreset",  handleSnifMapReset);
#endif
  S->on("/config", HTTP_GET,  handleConfigGet);
  S->on("/config", HTTP_POST, handleConfigPost);

  // Portal cautivo: cualquier ruta desconocida.  Si hay un AP en marcha (el
  // movil se acaba de conectar y su SO sondea una URL fija para "ver si hay
  // internet") -> redirigimos al panel y el SO abre la ventanita.  Sin AP
  // (modo solo-router) -> 404 normal.
  S->onNotFound([]() {
    IPAddress ap = WiFi.softAPIP();
    if ((uint32_t)ap != 0) {
      String u = "http://" + ap.toString() + "/";
      S->sendHeader("Location", u, true);
      S->sendHeader("Cache-Control", "no-store");
      S->send(302, "text/html",
        "<!doctype html><meta http-equiv=refresh content='0;url=" + u + "'>"
        "<body style='background:#111;color:#eee;font-family:system-ui;text-align:center;padding:40px'>"
        "<a href='" + u + "' style='color:#5ac8d8;font-size:18px'>Abrir panel de la balanza</a></body>");
    } else {
      S->send(404, "text/plain", "no existe");
    }
  });
}
