/*
 * LA PAGINA WEB - va embebida en el firmware, no en SPIFFS/LittleFS
 * -----------------------------------------------------------------------------
 * Se eligio embeberla como string a proposito. Con SPIFFS/LittleFS habria que
 * acordarse de correr "Upload Filesystem Image" como paso aparte del "Upload"
 * normal, y el dia que alguien se olvida el robot arranca sirviendo la version
 * vieja de la pagina -o un 404- sin ningun error visible. Embebida, la pagina y
 * el firmware siempre viajan juntos y no puede haber desfasaje.
 *
 * El costo es que ocupa flash de programa, pero la particion es huge_app (3MB)
 * y esto son ~14KB: no se nota.
 *
 * PROGMEM la deja en flash y no en RAM. AsyncWebServer la sirve con send_P(),
 * que la lee de a pedazos directamente de ahi.
 * -----------------------------------------------------------------------------
 */

#pragma once

#include <Arduino.h>
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<!-- user-scalable=no + viewport-fit: el joystick se maneja con el dedo, y sin
     esto un doble toque rapido hace zoom en vez de pegar un puñetazo. -->
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#0a0e14">
<title>WALL-E</title>
<style>
  :root{
    --bg:#0a0e14; --panel:#121822; --line:#22303f;
    --txt:#d7e3ee; --dim:#7d8fa1;
    --cy:#25d0d8; --cy-dim:#12656b;
    --warn:#ffb020; --bad:#ff4d5e; --ok:#3ddc84;
  }
  *{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
  html,body{margin:0;height:100%}
  body{
    background:var(--bg); color:var(--txt);
    font:600 15px/1.35 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
    display:flex; flex-direction:column;
    padding:env(safe-area-inset-top) 10px env(safe-area-inset-bottom);
    overflow:hidden; touch-action:manipulation;
  }

  /* ---------- encabezado ----------
     La distancia y el estado viven ACA y no en tarjetas aparte. Ocupaban una
     fila entera de 58px, y esa fila era justo lo que le faltaba al joystick
     para entrar en un celular de pantalla corta sin quedar diminuto. */
  header{display:flex;align-items:center;gap:6px;padding:8px 2px 6px}
  .logo{font-size:13px;letter-spacing:.14em;color:var(--cy);flex:none}
  .chip{flex:1;min-width:0;background:var(--panel);border:1px solid var(--line);
        border-radius:9px;padding:5px 7px;font-size:13px;color:var(--cy);
        white-space:nowrap;overflow:hidden;text-overflow:ellipsis;text-align:center}
  .chip.alert{color:var(--bad);border-color:#5c1a22}
  .dot{width:11px;height:11px;border-radius:50%;background:var(--bad);flex:none;
       box-shadow:0 0 8px currentColor;color:var(--bad)}
  .dot.on{background:var(--ok);color:var(--ok)}

  /* ---------- selector de modo ---------- */
  .tabs{display:flex;gap:8px;background:var(--panel);border:1px solid var(--line);
        border-radius:14px;padding:5px;margin-bottom:8px}
  .tab{flex:1;padding:13px 2px;text-align:center;border-radius:10px;
       color:var(--dim);font-size:12px;letter-spacing:.04em;
       border:none;background:none;font-weight:700}
  .tab.sel{background:var(--cy-dim);color:#eafeff;box-shadow:inset 0 0 0 1px var(--cy)}
  /* OFF es el modo seguro: cuando esta activo se pinta distinto, para que de un
     vistazo se sepa que el robot no se va a mover. */
  #tabOff.sel{background:#4a3a12;color:#ffe9b8;box-shadow:inset 0 0 0 1px var(--warn)}

  /* ---------- paneles ---------- */
  /* overflow:hidden no es decorativo: sin el, en pantallas cortas el contenido
     del panel se dibuja POR ENCIMA del boton de mute del pie, y quedan dos
     botones pisados uno sobre el otro. */
  .page{flex:1;min-height:0;display:none;flex-direction:column;gap:8px;overflow:hidden}
  .page.sel{display:flex}

  /* ---------- joystick ---------- */
  /* El joystick se mide por ALTURA y deduce el ancho del aspect-ratio, en vez de
     al reves. Asi es el unico elemento que se encoge cuando la pantalla es
     corta, y los botones de abajo nunca terminan tapados por el pie. Los max-
     evitan que en una tablet quede un joystick gigante. */
  #padwrap{flex:1 1 0;min-height:0;display:flex;align-items:center;justify-content:center}
  #pad{position:relative;flex:none;height:100%;width:auto;aspect-ratio:1;
       max-height:220px;max-width:min(62vw,220px);border-radius:50%;
       background:radial-gradient(circle at 50% 50%,#16202c 0%,#0d141d 72%);
       border:2px solid var(--line);touch-action:none}
  #pad::before,#pad::after{content:"";position:absolute;background:var(--line)}
  #pad::before{left:8%;right:8%;top:50%;height:1px}
  #pad::after{top:8%;bottom:8%;left:50%;width:1px}
  #knob{position:absolute;width:38%;height:38%;left:31%;top:31%;border-radius:50%;
        background:var(--cy);box-shadow:0 0 22px rgba(37,208,216,.55);
        transition:transform .06s linear}

  /* ---------- brazos ---------- */
  .arm{background:var(--panel);border:1px solid var(--line);border-radius:12px;
       padding:8px 10px}
  .arm .row{display:flex;align-items:center;gap:9px}
  .arm label{font-size:11px;letter-spacing:.1em;color:var(--dim);width:74px}
  .arm .val{margin-left:auto;color:var(--cy);font-size:14px;width:46px;text-align:right}
  .step{width:46px;height:44px;border-radius:11px;border:1px solid var(--line);
        background:#1a2431;color:var(--txt);font-size:21px;font-weight:700;flex:none}
  input[type=range]{flex:1;-webkit-appearance:none;height:44px;background:none;margin:0}
  input[type=range]::-webkit-slider-runnable-track{height:8px;border-radius:4px;background:#1e2a38}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:34px;height:34px;
        border-radius:50%;background:var(--cy);margin-top:-13px;border:none}
  input[type=range]::-moz-range-track{height:8px;border-radius:4px;background:#1e2a38}
  input[type=range]::-moz-range-thumb{width:34px;height:34px;border-radius:50%;
        background:var(--cy);border:none}

  /* ---------- botones grandes ---------- */
  .big{width:100%;padding:19px 0;border-radius:14px;border:1px solid var(--line);
       background:#1a2431;color:var(--txt);font-size:15px;font-weight:700;
       letter-spacing:.1em}
  .big:active{filter:brightness(1.45)}
  .big.box{background:linear-gradient(180deg,#7a2230,#4a141d);border-color:#a13445;color:#ffd9de}
  .big.talk{background:linear-gradient(180deg,#12626a,#0c3d43);border-color:var(--cy);color:#eafeff}
  .big[disabled]{opacity:.4}

  /* ---------- dialogo ---------- */
  #log{flex:1;min-height:80px;overflow-y:auto;background:var(--panel);
       border:1px solid var(--line);border-radius:12px;padding:10px;
       font-weight:500;font-size:14px;-webkit-overflow-scrolling:touch}
  #log p{margin:0 0 8px}
  #log .yo{color:var(--dim)}
  #log .ro{color:var(--cy)}
  #log .er{color:var(--warn)}

  /* ---------- pie: mute (vive fuera de las pestañas, aplica a los dos modos) ---------- */
  footer{padding:8px 0 12px}
  #mute{width:100%;padding:17px 0;border-radius:14px;font-size:15px;font-weight:700;
        letter-spacing:.08em;border:1px solid var(--line);background:#1a2431;color:var(--txt)}
  #mute.off{background:linear-gradient(180deg,#7a2230,#4a141d);border-color:#a13445;color:#ffd9de}

  /* ---------- linea de estado: en que anda el robot ahora ---------- */
  #phase{background:var(--panel);border:1px solid var(--line);border-radius:12px;
         padding:8px 11px;margin-bottom:6px;font-size:13px;font-weight:600;
         color:var(--dim);display:flex;align-items:center;gap:8px;min-height:38px}
  #phase.act{color:var(--cy);border-color:var(--cy-dim)}
  #phase.err{color:var(--warn);border-color:#5c4212}
  #phase .spin{width:12px;height:12px;border-radius:50%;flex:none;
               border:2px solid var(--cy);border-top-color:transparent;
               animation:sp .7s linear infinite;display:none}
  #phase.act .spin{display:block}
  @keyframes sp{to{transform:rotate(360deg)}}

  /* ---------- tira de voz: vive fuera de las pestañas ----------
     El microfono tiene que poder usarse en CUALQUIER modo, no solo en el
     autonomo: por eso este bloque esta afuera de las paginas y se ve siempre. */
  #voice{display:flex;gap:8px;margin-bottom:6px}
  #btnTalk{flex:2;padding:15px 0;border-radius:12px;font-size:14px;font-weight:700;
           letter-spacing:.06em;background:linear-gradient(180deg,#12626a,#0c3d43);
           border:1px solid var(--cy);color:#eafeff}
  #btnTalk.rec{background:linear-gradient(180deg,#7a2230,#4a141d);border-color:#a13445;
               color:#ffd9de}
  #btnTalk[disabled]{opacity:.4}
  #micSrc{flex:1;padding:15px 0;border-radius:12px;font-size:11px;font-weight:700;
          letter-spacing:.04em;background:#1a2431;border:1px solid var(--line);
          color:var(--txt)}
  #micSrc.phone{border-color:var(--warn);color:var(--warn)}

  /* ---------- escribir en vez de hablar (ultimo respaldo) ---------- */
  .say{display:flex;gap:8px}
  .say input{flex:1;min-width:0;padding:14px 12px;border-radius:12px;font-size:15px;
             background:#111a24;border:1px solid var(--line);color:var(--txt)}
  .say button{flex:none;width:64px;border-radius:12px;font-size:18px;font-weight:700;
              background:#1a2431;border:1px solid var(--line);color:var(--cy)}

  /* ---------- barra de nivel del microfono ---------- */
  .meter{position:relative;height:22px;border-radius:7px;background:#111a24;
         border:1px solid var(--line);overflow:hidden;margin:8px 0 4px}
  .meter.mini{height:8px;margin:0 0 8px;border-radius:4px}
  .meter i{position:absolute;left:0;top:0;bottom:0;width:0;background:var(--cy);
           transition:width .07s linear}
  .meter i.hot{background:var(--ok)}
  /* la marca es el nivel que hace falta para que cuente como voz */
  .meter u{position:absolute;top:0;bottom:0;width:2px;background:var(--warn);left:0}
  .hint{font-size:11px;font-weight:500;color:var(--dim);line-height:1.4}

  /* ---------- panel de diagnostico ---------- */
  #rows{background:var(--panel);border:1px solid var(--line);border-radius:12px;
        padding:4px 10px;overflow-y:auto;flex:1;min-height:70px}
  .r{display:flex;gap:8px;padding:7px 0;border-bottom:1px solid #1a2431;font-size:12px}
  .r:last-child{border-bottom:none}
  .r b{flex:none;width:104px;color:var(--dim);letter-spacing:.05em;font-weight:700}
  .r span{flex:1;font-weight:500;word-break:break-word}
  .r.ok span{color:var(--ok)}
  .r.bad span{color:var(--bad)}
  .r.inf span{color:var(--txt)}
  .grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  .grid2 .big{padding:15px 0;font-size:13px}
</style>
</head>
<body>

<header>
  <span class="logo">WALL-E</span>
  <span class="chip" id="dist">--</span>
  <span class="chip" id="face">--</span>
  <span id="dot" class="dot"></span>
</header>

<div class="tabs">
  <button class="tab sel" id="tabOff">OFF</button>
  <button class="tab" id="tabCtl">CONTROL</button>
  <button class="tab" id="tabAuto">AUTO</button>
  <button class="tab" id="tabDiag">DIAG</button>
</div>

<!-- En que anda el robot AHORA. Se ve en las tres pestañas a proposito: el
     estado importa igual estes manejando, charlando o diagnosticando. -->
<div id="phase"><span class="spin"></span><span id="phaseTxt">Esperando al robot...</span></div>

<!-- Voz: siempre visible, en los tres modos. -->
<div id="voice">
  <button id="btnTalk">HABLAR</button>
  <button id="micSrc">MIC: ROBOT</button>
</div>
<div class="meter mini"><i id="lvl"></i><u id="lvlTh"></u></div>

<!-- ===================== MODO CONTROL ===================== -->
<div class="page sel" id="pgCtl">
  <div id="padwrap">
    <div id="pad"><div id="knob"></div></div>
  </div>

  <div class="arm">
    <div class="row">
      <label>BRAZO IZQ</label>
      <button class="step" data-arm="l" data-d="-10">-</button>
      <input type="range" id="armL" min="-10" max="90" step="1" value="0">
      <button class="step" data-arm="l" data-d="10">+</button>
      <span class="val" id="armLv">0</span>
    </div>
    <div class="row" style="margin-top:6px">
      <label>BRAZO DER</label>
      <button class="step" data-arm="r" data-d="-10">-</button>
      <input type="range" id="armR" min="-10" max="90" step="1" value="0">
      <button class="step" data-arm="r" data-d="10">+</button>
      <span class="val" id="armRv">0</span>
    </div>
  </div>

  <button class="big box" id="btnBox">MODO BOXEO</button>
</div>

<!-- ===================== OFF y AUTONOMO comparten pagina =====================
     Los dos modos hacen lo mismo desde el celular: charlar. La unica diferencia
     es si el robot ademas se mueve, y eso lo dice la nota de arriba. -->
<div class="page sel" id="pgTalk">
  <div class="hint" id="modeNote"></div>
  <div class="say">
    <input id="sayTxt" placeholder="...o escribile aca" autocomplete="off"
           enterkeyhint="send" maxlength="150">
    <button id="saySend">&#9654;</button>
  </div>
  <div id="log"><p class="yo">El robot arranca en OFF: quieto hasta que elijas
    CONTROL o AUTO. Hablarle y escribirle funciona en cualquiera de los tres.</p></div>
</div>

<!-- ===================== DIAGNOSTICO ===================== -->
<div class="page" id="pgDiag">
  <div class="hint">La barra de arriba es el nivel del microfono; la marca
    amarilla, el minimo para que cuente como voz. Si no se mueve al hablar, el
    problema esta en el microfono y no en la red.</div>

  <!-- SONIDO ROBOT va primero y ancho: es el unico test que no usa internet ni
       gasta creditos, asi que es por el que hay que empezar siempre. -->
  <button class="big talk" data-test="tone">SONIDO ROBOT (SIN INTERNET)</button>

  <div class="grid2">
    <button class="big" data-test="mic">PROBAR MIC</button>
    <button class="big" data-test="arms">PROBAR BRAZOS</button>
    <button class="big" data-test="speaker">PROBAR VOZ TTS</button>
    <button class="big" data-test="oled">PROBAR PANTALLA</button>
  </div>

  <div id="rows"><div class="r inf"><b>...</b><span>esperando el reporte del robot</span></div></div>
</div>

<footer>
  <button id="mute">MICROFONO ACTIVO</button>
</footer>

<script>
// ===================== WEBSOCKET =====================
// Se usa WebSocket y no polling HTTP: el joystick manda ~20 posiciones por
// segundo y con un fetch() por posicion el ESP32 se pasaria la vida abriendo y
// cerrando sockets. Aca la conexion queda abierta y cada mensaje son 30 bytes.
let ws, alive = false;

function connect(){
  ws = new WebSocket("ws://" + location.host + "/ws");
  ws.onopen  = () => { alive = true;  dot.classList.add("on"); };
  ws.onclose = () => {
    alive = false; dot.classList.remove("on");
    setPhase("err", "Sin conexion con el robot. Reintentando...");
    setTimeout(connect, 1200);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = e => { try { onState(JSON.parse(e.data)); } catch(_){} };
}
function send(o){ if (alive && ws.readyState === 1) ws.send(JSON.stringify(o)); }

const $ = id => document.getElementById(id);
const dot = $("dot");

// ===================== ESTADO QUE LLEGA DEL ROBOT =====================
const CARAS = { off:"APAGADO", idle:"EN REPOSO", happy:"CONTENTO",
                alert:"OBSTACULO", listening:"ESCUCHANDO", thinking:"PENSANDO",
                talking:"HABLANDO", control:"CONTROL", boxing:"BOXEANDO" };

const NOTAS = {
  off:  "Modo OFF: el robot no mueve motores ni brazos. Igual te escucha, te "
      + "contesta en voz alta y anima la boca.",
  auto: "Modo AUTONOMO: anda solo esquivando obstaculos, y cuando le hablas se "
      + "queda quieto para contestarte."
};

// Texto por defecto de cada etapa del turno de voz. El robot casi siempre manda
// un detalle mas especifico; esto es el respaldo.
const FASES = { conn:"Conectando...", wait:"Te escucho, habla ahora",
                rec:"Grabando", stt:"Transcribiendo", llm:"Pensando",
                tts:"Generando la voz", talk:"Hablando", test:"Probando",
                idle:"", err:"" };

function setPhase(p, d){
  const el = $("phase");
  $("phaseTxt").textContent = d || FASES[p] || p;
  el.className = (p === "err") ? "err" : (p && p !== "idle" ? "act" : "");
  if (!d && (!p || p === "idle")) $("phaseTxt").textContent = "Listo.";
}

function onState(m){
  if (m.t === "st"){
    $("dist").textContent = m.dist >= 999 ? "libre" : m.dist + " cm";
    $("dist").className = (m.dist < 20) ? "chip alert" : "chip";
    $("face").textContent = CARAS[m.face] || m.face;
    setMuteUi(m.mute);
    setSrcUi(m.src);
    // La pestaña DIAG no es un modo: mirar el diagnostico no tiene por que
    // sacarte de ahi cada vez que llega telemetria.
    if (!$("tabDiag").classList.contains("sel")) setTab(m.mode, false);
    // El robot es la fuente de verdad de la posicion de los brazos: los mueve
    // solo mientras habla o boxea. Los sliders lo siguen, salvo mientras el
    // dedo esta encima de uno (ahi mandaria el robot y el slider saltaria).
    if (!dragging){
      setArmUi("L", m.armL);
      setArmUi("R", m.armR);
    }
    // Con el microfono del celular el boton lo maneja el navegador: si se
    // deshabilitara por "busy" no se podria cortar una grabacion empezada.
    $("btnTalk").disabled = (m.busy && micSrc === "robot") || m.mute;
    $("btnBox").disabled  = m.busy;
  } else if (m.t === "ph"){
    setPhase(m.p, m.d);
    // Un error tambien queda escrito en el dialogo: la linea de estado la pisa
    // el mensaje siguiente, y despues no queda rastro de que fue lo que fallo.
    if (m.p === "err" && m.d) onState({t:"log", who:"er", msg:m.d});

  } else if (m.t === "mic"){
    // Escala hasta 6000: por arriba de eso ya es una voz fuerte y no aporta
    // seguir estirando la barra.
    const pct = Math.min(100, m.v / 60);
    const bar = $("lvl");
    bar.style.width = pct + "%";
    bar.className = (m.th > 0 && m.v > m.th) ? "hot" : "";
    $("lvlTh").style.left = Math.min(100, m.th / 60) + "%";
    $("lvlTh").style.display = m.th > 0 ? "block" : "none";

  } else if (m.t === "diag"){
    const box = $("rows");
    box.innerHTML = "";
    for (const [k, v, ok] of m.rows){
      const d = document.createElement("div");
      d.className = "r " + (ok === 1 ? "ok" : ok === 0 ? "bad" : "inf");
      d.innerHTML = "<b></b><span></span>";
      d.children[0].textContent = k;
      d.children[1].textContent = v;
      box.appendChild(d);
    }

  } else if (m.t === "log"){
    const p = document.createElement("p");
    p.className = m.who === "yo" ? "yo" : (m.who === "er" ? "er" : "ro");
    p.textContent = (m.who === "yo" ? "VOS: " : m.who === "er" ? "" : "ROBOT: ") + m.msg;
    const l = $("log");
    l.appendChild(p);
    l.scrollTop = l.scrollHeight;
  }
}

// ===================== PESTAÑAS / MODO =====================
// DIAG no es un modo del robot, es solo una vista: mirar el diagnostico no
// tiene por que cambiar lo que el robot esta haciendo. Por eso solo CONTROL y
// AUTONOMO mandan el cambio de modo, y la telemetria no fuerza la pestaña
// mientras estas en DIAG.
// Tres modos del robot (off / control / auto) y una vista que no es un modo
// (diag). OFF y AUTO comparten la misma pagina, que es la de charlar.
function setTab(which, tell){
  if (which === "control") which = "ctl";
  const pag = (which === "ctl") ? "pgCtl" : (which === "diag") ? "pgDiag" : "pgTalk";

  for (const [id, t] of [["tabOff","off"],["tabCtl","ctl"],["tabAuto","auto"],["tabDiag","diag"]])
    $(id).classList.toggle("sel", t === which);
  for (const pg of ["pgTalk","pgCtl","pgDiag"])
    $(pg).classList.toggle("sel", pg === pag);

  if (which === "off" || which === "auto") $("modeNote").textContent = NOTAS[which];
  if (tell && which !== "diag")
    send({t:"mode", v: which === "ctl" ? "control" : which});
}
$("tabOff").onclick  = () => setTab("off", true);
$("tabCtl").onclick  = () => setTab("ctl", true);
$("tabAuto").onclick = () => setTab("auto", true);
$("tabDiag").onclick = () => setTab("diag", false);

// ===================== JOYSTICK =====================
// Hecho a mano en vez de nipple.js: son 40 lineas, no hay que servir una
// libreria de 30KB desde la flash del ESP32, y no depende de internet (el
// celular esta en la red del robot, que puede no tener salida).
const pad = $("pad"), knob = $("knob");
let jx = 0, jy = 0, jTouch = null, jLast = 0;

function jSet(x, y){                       // x,y en -1..1
  jx = Math.round(x * 100);
  jy = Math.round(y * 100);
  knob.style.transform = "translate(" + (x*60) + "%," + (-y*60) + "%)";
}

function jFrom(ev){
  const t = [...ev.touches].find(t => t.identifier === jTouch) || ev.touches[0];
  if (!t) return;
  const r = pad.getBoundingClientRect();
  let x = (t.clientX - r.left - r.width/2)  / (r.width/2);
  let y = -(t.clientY - r.top - r.height/2) / (r.height/2);
  const m = Math.hypot(x, y);
  if (m > 1){ x /= m; y /= m; }            // el knob no sale del circulo
  jSet(x, y);
}

pad.addEventListener("touchstart", e => {
  e.preventDefault(); jTouch = e.changedTouches[0].identifier; jFrom(e); jPush(true);
}, {passive:false});
pad.addEventListener("touchmove", e => { e.preventDefault(); jFrom(e); }, {passive:false});
function jEnd(e){ e.preventDefault(); jTouch = null; jSet(0,0); jPush(true); }
pad.addEventListener("touchend", jEnd, {passive:false});
pad.addEventListener("touchcancel", jEnd, {passive:false});

// Y con mouse, para poder probar desde la compu.
pad.addEventListener("mousedown", e => { jTouch = -1; jMouse(e); jPush(true); });
window.addEventListener("mousemove", e => { if (jTouch === -1) jMouse(e); });
window.addEventListener("mouseup",   e => { if (jTouch === -1){ jTouch = null; jSet(0,0); jPush(true); } });
function jMouse(e){
  const r = pad.getBoundingClientRect();
  let x = (e.clientX - r.left - r.width/2)  / (r.width/2);
  let y = -(e.clientY - r.top - r.height/2) / (r.height/2);
  const m = Math.hypot(x, y);
  if (m > 1){ x /= m; y /= m; }
  jSet(x, y);
}

// El joystick se manda a ritmo fijo (20Hz) y no en cada evento de touchmove:
// un dedo genera 60 a 120 eventos por segundo y el ESP32 no necesita tanto.
// Ademas el robot usa la llegada de estos mensajes como señal de vida: si
// dejan de llegar frena solo, asi que se mandan tambien cuando no hubo cambio.
function jPush(force){
  const now = Date.now();
  if (!force && now - jLast < 50) return;
  jLast = now;
  send({t:"joy", x:jx, y:jy});
}
setInterval(() => { if (document.getElementById("pgCtl").classList.contains("sel")) jPush(false); }, 50);

// ===================== BRAZOS =====================
let dragging = false, armLast = 0;

function setArmUi(side, deg){
  if (deg === undefined || deg === null) return;
  $("arm" + side).value = deg;
  $("arm" + side + "v").textContent = deg + "°";
}

function armPush(side, deg, force){
  deg = Math.max(-10, Math.min(90, Math.round(deg)));
  setArmUi(side, deg);
  const now = Date.now();
  if (!force && now - armLast < 60) return;
  armLast = now;
  send({t:"arm", a:side.toLowerCase(), v:deg});
}

["L","R"].forEach(s => {
  const el = $("arm" + s);
  el.addEventListener("input",   () => armPush(s, +el.value, false));
  el.addEventListener("pointerdown", () => dragging = true);
  el.addEventListener("change",  () => { dragging = false; armPush(s, +el.value, true); });
});
document.querySelectorAll(".step").forEach(b => {
  b.onclick = () => {
    const s = b.dataset.arm.toUpperCase();
    armPush(s, (+$("arm" + s).value) + (+b.dataset.d), true);
  };
});

// ===================== BOTONES =====================
$("btnBox").onclick = () => send({t:"box"});

// ===================== DE DONDE SALE LA VOZ =====================
//
// Dos fuentes: el microfono del robot (el que graba el ESP32) y el del celular
// (lo graba el navegador y le sube el archivo al robot). El segundo es el
// respaldo para cuando el del robot no anda.
let micSrc = "robot";

function setSrcUi(v){
  if (v) micSrc = v;
  const b = $("micSrc");
  b.textContent = micSrc === "phone" ? "MIC: CELU" : "MIC: ROBOT";
  b.classList.toggle("phone", micSrc === "phone");
}

$("micSrc").onclick = () => {
  const nuevo = micSrc === "robot" ? "phone" : "robot";
  if (nuevo === "phone" && !puedeGrabar()){
    setPhase("err", "Este navegador no deja usar el microfono por HTTP. Mira DIAG.");
    onState({t:"log", who:"er", msg:
      "El microfono del celular necesita conexion segura (https) y esta pagina " +
      "va por http. En Chrome de Android se habilita entrando a " +
      "chrome://flags/#unsafely-treat-insecure-origin-as-secure y agregando " +
      "http://" + location.host + " a la lista. En iPhone no hay equivalente: " +
      "ahi el respaldo es escribirle el texto."});
    return;
  }
  setSrcUi(nuevo);
  send({t:"micsrc", v:nuevo});
};

function puedeGrabar(){
  return !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia &&
            window.MediaRecorder);
}

// El boton HABLAR hace dos cosas distintas segun la fuente: con el microfono
// del robot solo avisa y graba el ESP32; con el del celular graba ACA y despues
// sube el archivo.
$("btnTalk").onclick = () => {
  if (micSrc === "phone") grabarConCelular();
  else send({t:"talk"});
};

// ===================== GRABACION CON EL CELULAR =====================
let rec = null;

function formatoGrabacion(){
  // El servicio de transcripcion deduce el formato de la extension, asi que hay
  // que decirle al robot con que se grabo. Android/Chrome dan webm/opus;
  // Safari da mp4.
  for (const [mime, fmt] of [["audio/webm;codecs=opus","webm"], ["audio/webm","webm"],
                             ["audio/ogg;codecs=opus","ogg"], ["audio/mp4","mp4"]])
    if (window.MediaRecorder && MediaRecorder.isTypeSupported(mime)) return [mime, fmt];
  return ["", "webm"];
}

async function grabarConCelular(){
  if (rec && rec.state === "recording"){ rec.stop(); return; }
  if (!puedeGrabar()){
    setPhase("err", "Este navegador no permite grabar por HTTP. Mira DIAG.");
    return;
  }
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia(
      {audio:{echoCancellation:true, noiseSuppression:true}});
  } catch(e){
    setPhase("err", "No se pudo abrir el microfono: " + e.name);
    return;
  }

  const [mime, fmt] = formatoGrabacion();
  const trozos = [];
  rec = new MediaRecorder(stream, mime ? {mimeType:mime} : undefined);
  rec.ondataavailable = e => { if (e.data && e.data.size) trozos.push(e.data); };

  rec.onstop = async () => {
    stream.getTracks().forEach(t => t.stop());
    $("btnTalk").classList.remove("rec");
    $("btnTalk").textContent = "HABLAR";
    rec = null;

    const blob = new Blob(trozos, {type: mime || "application/octet-stream"});
    const kb = Math.round(blob.size / 1024);
    if (!blob.size){ setPhase("err", "La grabacion salio vacia."); return; }

    setPhase("stt", "Subiendo " + kb + " KB al robot...");
    try {
      const r = await fetch("/mic?fmt=" + fmt, {method:"POST", body:blob});
      if (!r.ok) setPhase("err", "El robot rechazo el audio (" + r.status + "): " +
                                 (await r.text()));
    } catch(e){
      setPhase("err", "No se pudo subir el audio: " + e.message);
    }
  };

  rec.start();
  $("btnTalk").classList.add("rec");
  $("btnTalk").textContent = "CORTAR";
  setPhase("rec", "Grabando con el celular... (tocá CORTAR para terminar)");
  // Tope duro, por si el usuario deja la pagina y no corta nunca.
  setTimeout(() => { if (rec && rec.state === "recording") rec.stop(); }, 8000);
}

// ===================== ESCRIBIRLE =====================
function enviarTexto(){
  const el = $("sayTxt");
  const txt = el.value.trim();
  if (!txt) return;
  send({t:"say", txt:txt});
  el.value = "";
}
$("saySend").onclick = enviarTexto;
$("sayTxt").addEventListener("keydown", e => { if (e.key === "Enter") enviarTexto(); });

document.querySelectorAll("[data-test]").forEach(b => {
  b.onclick = () => send({t:"test", w:b.dataset.test});
});

let muted = false;
function setMuteUi(v){
  muted = !!v;
  const b = $("mute");
  b.textContent = muted ? "MICROFONO SILENCIADO" : "MICROFONO ACTIVO";
  b.classList.toggle("off", muted);
}
$("mute").onclick = () => { setMuteUi(!muted); send({t:"mute", v:muted}); };

// Si el celular se bloquea o se cambia de app, el navegador congela el
// setInterval y el robot deja de recibir el joystick: frena solo por el
// watchdog. Al volver, mandamos un cero explicito para no arrancar de golpe
// con la ultima posicion que habia quedado.
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible"){ jSet(0,0); jPush(true); }
});

setSrcUi("robot");
setTab("off", false);
if (!puedeGrabar()) $("micSrc").classList.add("phone");
connect();
</script>
</body>
</html>
)HTMLPAGE";
