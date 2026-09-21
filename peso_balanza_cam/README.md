# peso_balanza_cam — ESP32-S3: OCR con cámara y/o sniffer del bus + web

Lee el **PESO** de la balanza cuenta-precio por **cámara**, por **sniffer del bus
del display**, o por **los dos a la vez** (para compararlos), y lo publica por WiFi.

Placa: **ESP32-S3 Super Mini** (chip ESP32-S3FH4R2: 4 MB flash + 2 MB PSRAM Quad,
un solo USB-C nativo, sin CH340). Antes se usaba un ESP32-S3-WROOM (N16R8) con
conector FFC de cámara y doble USB-C; se cambió de módulo por daño de hardware
(ver la sección 6, pines del sniffer).

---

## 1. Elegir el método — `config.h`

```c
#define ENABLE_OCR       1     // leer el display con la cámara
#define ENABLE_SNIFFER   0     // escuchar el bus DA/SL del display
#define OCR_AUTO         1     // 1 = localiza los dígitos solo (sin encuadrar)
```

| OCR | SNIFFER | resultado |
|:---:|:---:|---|
| 1 | 0 | **solo cámara** — no hay que soldar ni abrir la balanza *(empieza por aquí)* |
| 0 | 1 | solo sniffer del bus |
| 1 | 1 | los dos; el panel web los compara |

Lo desactivado **no se compila** (ni reserva pines ni RAM).

---

## 2. Compilar (Arduino IDE)

- esp32 (Espressif) ≥ 3.0 · Placa: **ESP32S3 Dev Module** (genérico, sirve para el Super Mini)
- **PSRAM: `Disabled`** (o `QSPI PSRAM` si el menú obliga a elegir tipo) — con
  `ENABLE_OCR 0` no hace falta, era solo para los buffers de la cámara
- Flash Size: `4MB (32Mb)` · Partition: cualquiera pensado para 4MB (p.ej.
  `Minimal SPIFFS (1.9MB APP/190KB SPIFFS)`) — confirma que el binario entra
- USB CDC On Boot: **`Enabled`** — esta placa NO tiene CH340, es un solo
  USB-C nativo; con `Disabled` no sale nada por el Monitor Serie
- Monitor Serie **115200** → sale la IP / la red creada.

### WiFi — `config.h`

```c
#define WIFI_MODE  2     // 0 = SOLO AP (campo) · 1 = SOLO router · 2 = AP + router
```

| Modo | Qué hace |
|:---:|---|
| **0** | El ESP **crea su red** `WIFI_AP_SSID` / `WIFI_AP_PASS`. Panel en **`http://192.168.4.1/`**. Para campo, sin router. |
| 1 | Se une al router (`WIFI_SSID`/`WIFI_PASS`). Si no puede en 15 s y `WIFI_AP_FALLBACK 1`, crea su red. |
| **2** *(def)* | Las dos: su red **siempre**, y además se une al router si lo ve (sin bloquear). Sirve para oficina y campo. |

**Portal cautivo** (`WIFI_CAPTIVE 1`): al conectar el móvil a la red del ESP, el
sistema operativo abre solo el panel (como el login de un aeropuerto). Lo hace un
DNS "atrápalo-todo" en el ESP + un redirect a `192.168.4.1` para cualquier URL
desconocida. Si el mini-navegador del móvil va lento, ciérralo y abre el navegador
normal → `192.168.4.1`.

Sin internet no hay NTP: el **móvil del operario pone la hora** al abrir el panel
(`/settime`), así el historial lleva hora real. Si tampoco, queda el orden y
"hace N s" de la sesión.

### Pines de la cámara

`config.h` trae el pinout **ESP32-S3-EYE** (el que copian casi todas las placas
"ESP32-S3-WROOM CAM"). Si al arrancar sale `esp_camera_init fallo 0x...`, busca el
pinout exacto de tu placa y corrige los `CAM_PIN_*`.

### Pines del sniffer (solo si `ENABLE_SNIFFER 1`)

| Balanza | | ESP32-S3 |
|---|---|---|
| `SL` | `— [470 Ω] —` | `GPIO1` |
| `DA` | `— [470 Ω] — [diodo Schottky] —` | `GPIO4` |
| `GND` | `———` | `GND` (masa común **obligatoria**) |

DA lleva además un **diodo Schottky** en serie (ánodo hacia la balanza, cátodo
hacia el GPIO) — protege contra una fuga interna del ESP devolviéndose hacia el
bus de la balanza (así se descubrió que el módulo WROOM anterior tenía 2 GPIO
con fuga hacia el riel de 5V, y terminó reemplazándose).

No conectar `VCC_DIS`. `GPIO1` y `GPIO4` están limpios en esta placa; **no
`GPIO2`** (tiene el LED "ON" de la placa, que carga la línea). Evitar UART0
(`43/44`), USB nativo (`19/20`) y strapping S3 (`0/3/45/46`) — son del silicio,
valen para cualquier placa. El ISR lee `GPIO_IN_REG` → usa pines ≤ 31.

---

## 3. Cuándo se guarda un pesaje — máquina de estados

`weighlog.cpp`, común a los dos métodos:

```
 VACIA ──(peso > WEIGH_ZERO_THRESH)──▶ CON CARGA
 CON CARGA (mientras el peso esté ESTABLE se RECUERDA su valor)
   └─ la plataforma cae por debajo del umbral ──▶ CONFIRMANDO (~WEIGH_CONFIRM_MS)
        · el peso BAJÓ de verdad (pasó por valores WEIGH_PARTIAL kg menores, o
          < WEIGH_PARTIAL_FRAC del estable si el neto es pequeño; con carga aún
          presente) ──────────────────────────────▶ se GUARDA  + weighOnCommit
        · salto LIMPIO a 0  ó  plataforma en NEGATIVO ──▶ es una TARA, NO se guarda
        · vuelve a haber carga (cambio de gaveta) ──▶ guarda el pendiente y sigue
```

- **Distingue una TARA de un retiro real** por *cómo* llega el peso a cero: un
  retiro hace oscilar la celda de carga (pasa por valores intermedios); una tara
  es un salto instantáneo y/o deja la plataforma en negativo.
- Un retiro **rápido** se sigue guardando (la celda siempre oscila al descargarse).
- Re-tarar a mitad de sesión funciona solo: ese salto a 0 se ignora, las
  siguientes gavetas se guardan normal.
- "Estable" = el **bit ESTABLE del bus** (punto verde del panel). Con OCR se usa
  "número quieto `WEIGH_STABLE_MS`".
- **PRECIO y TOTAL** se toman en cada muestra estable (si re-tecleas el precio con
  el peso puesto, se guarda el precio **nuevo**).
- Monitor Serie: `[pesaje] carga detectada...` · `[pesaje] retirado -> GUARDADO
  PESO 1.30 ...` · `[pesaje] TARA (...) -> pendiente descartado`.

Ajustes en `config.h`:

| Parámetro | Qué es | Por defecto |
|---|---|---|
| `WEIGH_ZERO_THRESH` | kg por debajo = plataforma vacía | `0.10` |
| `WEIGH_CONFIRM_MS` | tiempo de comprobación "¿retiro o tara?" antes de guardar | `1000` |
| `WEIGH_PARTIAL` | cuánto (kg) tiene que bajar el peso con carga para contar como retiro real | `0.15` |
| `WEIGH_PARTIAL_FRAC` | …o a qué fracción del valor estable (para netos pequeños con tara) | `0.55` |
| `WEIGH_ZERO_MS` | cuánto dura el negativo para confirmar una tara | `400` |
| `WEIGH_STABLE_MS` | (solo OCR) número quieto → "estable" | `700` |
| `WEIGH_INVALID_MS` | display apagado este tiempo → cierra | `2500` |
| `WEIGH_LOG_SIZE` | pesajes que guarda en memoria/NVS | `120` |
| `WEIGH_V2` | `0` = comportamiento anterior a la ronda ag (sin rebote/anti-meneo/commit rápido) | `1` |
| `WEIGH_DIP` | el neto rebota a menos de `-esto` al retirar de verdad (la celda rebota; una tara nunca baja de 0) | `0.12` |
| `WEIGH_SETTLE_MS` | con señal de retiro, guarda a los ~350 ms en vez de `WEIGH_CONFIRM_MS` | `350` |
| `WEIGH_FEED_MS` | cada cuánto se alimenta la máquina de estados (más muestras = pilla mejor el retiro rápido) | `20` |

Si un pesaje real se sigue perdiendo, mira el Monitor Serie: la línea
`cero limpio (dip X.XX) sin descarga` dice cuánto rebotó — baja `WEIGH_DIP`
hacia ese valor. Si alguna **tara** se cuela, súbelo. Para volver del todo al
comportamiento anterior: `WEIGH_V2 0`, o restaura `_backup_pre_ag/`.

- Panel `/`, pensado para el operario (orden fijo): **pantalla LED** (PESO
  grande, PRECIO UNITARIO e IMPORTE TOTAL debajo) + indicador ESTABLE →
  **objetivos "malla" / "BIN"** (dos campos donde el operario escribe un peso
  en lb; se alerta — color, vibración y pitido — en cuanto la lectura **en
  vivo** o el acumulado lo **igualan o superan**; el valor se recuerda en el
  navegador) → **último pesaje guardado** → **peso total acumulado** (suma de
  todos los pesajes desde el último *Borrar*, sobrevive a reinicios). El
  historial completo (lista deslizable, CSV, Borrar) queda plegado bajo
  **"Historial"**; ahí ya no se muestra info técnica (bus/WiFi/RSSI, enlaces a
  `/config` y `/sniffer`) — sigue disponible entrando a esas rutas directo.
  Estilo *glassmorphism*; botón sol/luna arriba a la derecha para **modo claro
  / oscuro** (se recuerda en el navegador). Sin fuentes web ni librerías —
  funciona sin internet.
- Cada pesaje guarda los **3 campos**: `peso`, `precio unitario`, `total`
  (el precio/total tal como los mostraba la balanza al estabilizarse el peso).
- **`/weighings.csv`**: descarga el historial (`n;peso;precio_unit;total;hora_local;hace_s`).
  En el panel, **"Copiar CSV"** lo pone en el portapapeles (útil en iPhone, donde
  la ventanita automática de la red no deja descargar archivos — para el archivo,
  abre Safari y entra a `192.168.4.1`).
- Cada fila del historial se **borra deslizándola** hacia la izquierda. El `#`
  de las filas se **renumera** (sin huecos) y el contador se ajusta al instante.
  `GET /weighings/del?id=N`.
- Botón **Borrar todo** → tira de confirmación (Cancelar / Sí, borrar) que
  aparece debajo sin mover el resto. También `GET/POST /weighings/clear`.
- El panel muestra **lb**. El firmware no convierte: el número es el que muestra
  la balanza → **ponla en libras**. Los umbrales `WEIGH_*` (`0.10`, `0.15`,
  `WEIGH_DIP 0.12`) son números absolutos; se interpretan en la unidad de la
  balanza (ahora libras).
- **El historial se guarda en NVS** (`WEIGH_PERSIST 1`): sobrevive a apagones /
  cambio de batería. Últimos `WEIGH_LOG_SIZE` (120). Al reiniciar se recarga y
  el Monitor Serie dice `historial cargado de NVS: N`.
- `weighOnCommit(cb)` en `weighlog.h`: gancho para el **envío al servidor**
  (pendiente) — salta 1 vez al confirmarse cada pesaje, con los 3 campos.

## 4. Rutas del servidor

| Ruta | | Qué hace |
|---|---|---|
| `/` | | panel (se adapta a lo que esté activo) |
| `/api` | | JSON estado en vivo |
| `/weighings` · `/weighings.csv` | | historial de pesajes (JSON / CSV) |
| `/weighings/clear` | | borra el historial (RAM + NVS) |
| `/settime?epoch=…` | | el móvil le pasa la hora al ESP (sin internet) |
| `/config` | | calibración OCR y/o mapa del sniffer (se guarda en NVS) |
| `/snapshot.jpg` · `/ocr_debug.jpg` | OCR | foto actual / con las zonas dibujadas |
| `/sniffer` · `/sniffer/raw` | SNIFFER | 16 bytes de RAM / volcado de flancos |

---

## 5. Puesta a punto del OCR

Por defecto va en **modo automático** (`auto=1`): **no hay que encuadrar nada**.
Cada frame (5×/s) localiza la **banda** de brillo (los LEDs sobre negro) y el
**borde derecho del último LED** (= borde del dígito de las unidades, que
siempre está encendido). A partir de ahí monta la rejilla:
`ancho_digito = alto_banda × asp` , `separacion = ancho_digito × pit`.

**Robusto a que la cámara se mueva:**

- `asp` y `pit` son **proporciones**, no píxeles → si la cámara se acerca/aleja,
  todo escala solo (digitos más grandes → banda más alta → celdas más anchas).
  Los mismos `asp`/`pit` valen a cualquier distancia.
- Traslación (unos cm arriba/abajo/lados): la banda y el borde derecho se
  re-miden en cada frame; la rejilla los sigue.
- Vibración: suavizado que filtra el temblor pero **reengancha de golpe** si el
  salto supera el 20% del alto del display.
- Si `autoLocate` falla un instante (reflejo, alguien pasa), mantiene la última
  rejilla hasta 3 s.
- Rotación pequeña (±2–3°): la absorbe el promediado de cada segmento. Rotación
  grande necesitaría montaje más rígido.

En `/config` todo se ajusta **en vivo**. La línea de estado muestra la lectura
cruda (`cruda "130"`) o el valor, nº de celdas y umbrales. En la imagen:
rectángulo **tenue** = zona detectada; rectángulos **claros** = celdas.

- Celdas anchas/estrechas → `asp` (probado: **0.62**).
- Celdas juntas/separadas → `pit` (probado: **1.50**).
- Segmentos mal → `umbral seg` (0–1). Torcidos → `inclinacion`.
- **Guardar** para que persista al reinicio.

### Si confunde cifras (3/5, 6/8...)

Un segmento realmente encendido está brillante **a lo largo de todo el trazo**;
el "sangrado" de un segmento vecino solo ilumina una punta. Por eso el segmento
"encendido" se decide por **cobertura** (`OCR_SEG_COVERAGE` % del trazo por
encima del umbral), no por brillo medio. Ajustes:

- Sigue confundiendo → **enfoca la lente** de la OV2640 (gírala). Con la imagen
  borrosa, un 5 y un 6 se parecen demasiado y ningún algoritmo lo arregla.
- `OCR_SEG_COVERAGE` (`config.h`): baja a ~45 si se deja segmentos sin detectar,
  sube a ~65 si detecta de más.
- `umbral seg` (`onR` en `/config`).

> `OCR_USE_TEMPLATES` (config.h, por defecto `0`): reconocimiento por forma
> (plantillas aprendidas). Experimental — muy sensible a que la rejilla auto se
> mueva. Se entrena en `/config` → "Plantillas". Déjalo en `0` salvo pruebas.

### Luz ambiente

Con el display a oscuras funciona directo. **Con luz de sala** el bisel blanco
brilla como los LEDs; el detector lo descarta por su forma, pero si aún confunde
pon **`brillo ON`** (`abr` en `/config`, o `OCR_AUTO_BRIGHT` en `config.h`) a
**~245** → solo cuentan los LEDs. En caja fija se pone una vez.

**Si el modo auto no engancha:** `auto=0` + caja manual (botones ←→↑↓).

---

## 6. Sniffer del bus (método alternativo, sin cámara)

En `config.h`: `#define ENABLE_SNIFFER 1` (puedes dejar `ENABLE_OCR 1` a la vez
para comparar). Cablea:

| Balanza (conector DIS) | | ESP32-S3 |
|---|---|---|
| `SL` | `— [470 Ω] —` | `GPIO1` |
| `DA` | `— [470 Ω] — [diodo Schottky] —` | `GPIO4` |
| `GND` | `———` | `GND` (masa común **obligatoria**) |

No conectes `VCC_DIS`. Balanza con su batería, placa por USB-C. Ver la sección
2 (pines del sniffer) para el detalle del diodo en la línea DA.

### Comprobar que capta

Abre `/sniffer`. Arriba hay una **réplica de la pantalla LED** (los 3 displays
con 7 segmentos) que debe reflejar lo que muestra la balanza. Debajo:
`tramas` subiendo, `SL ~X kHz media / Y kHz pico`. Si `Y` (pico) sale muy alto
(>300 kHz) pero baja al pulsar **"Reiniciar medida SL"** y "pulsos rápidos" se
queda en 0, era ruido.

Mapa por defecto (`config.h`): bytes **0-4 = PESO**, **5-9 = PRECIO UNITARIO**,
**10-15 = TOTAL**, segmentos estándar (`a`=bit0 … `g`=bit6, punto=bit7). Si tu
balanza coincide (probado con la `ACS-JC36CV28`), **no hay que resolver nada**.
El detalle hex/binario está en `/sniffer` → *"2. RAM del display (detalle
técnico)"*.

### Si marca `0 tramas / 0 refrescos / SL kHz` vacío

Cero flancos de reloj = problema **eléctrico**, no de software. En `/sniffer`,
sección **"0. Diagnóstico de cableado"**:

- Con la balanza encendida y el número cambiando (pon la mano en el plato), los
  contadores `flancos=` de SL y DA tienen que **subir**. Si siguen en 0:
- Pulsa **"Probar pines"**:
  - `FLOTANTE` → ese cable no llega a nada: pad equivocado, soldadura fría o
    hilo roto.
  - `ALTO fijo` → conectado y en reposo (bien) — el problema es que el bus no
    tiene tráfico (¿balanza en standby? ¿display apagado?).
  - `BAJO fijo` → cable a masa o a un segmento.
- Comprueba: **masa común** (multímetro en continuidad entre `GND` del ESP32 y
  `GND` de la balanza), resistencias de **470 Ω** (amarillo-violeta-**marrón**,
  no 470 k), y el pad correcto en el conector de la placa de display
  (`DIN`/`CLK`, a veces `SDA`/`SCL` o `DA`/`SL`). Puedes puentear directo sin
  resistencia para descartarlas (3 V y misma masa → es seguro para el GPIO).

### Si el mapa se descuadra (dígitos que faltan, PRECIO/TOTAL en blanco…)

El mapa `SNIF_*_ADDR` de `config.h` vale para la `ACS-JC36CV28`. Si un
**"Resolver mapa"** salió mal, quedó guardado en NVS y pisa al de `config.h`.
Arreglo:

- **Botón "Restaurar mapa por defecto"** en `/sniffer` → borra el de NVS.
- O sube `SNIF_MAP_VERSION` en `config.h` y reflashea → se borra solo.
- La línea `mapa PESO[…] PRECIO[…] TOTAL[…]` de `/sniffer` muestra el que hay.

### Nº de decimales (`SNIF_*_DEC` en `config.h`)

- **`>= 0`** → número FIJO de decimales, **ignorando el bit de punto del
  display**. Así un icono NET/TARA que encienda ese bit al tarar no descoloca
  el valor (`-10.00` y `1.60` salen bien, no `1000` ni `160`).
- **`-1`** → los decimales los marca el punto del propio display (para campos
  que pueden ser enteros *o* decimales).

`ACS-JC36CV28`: `SNIF_PESO_DEC 2`, `SNIF_TOTAL_DEC 2`, `SNIF_PRECIO_DEC -1`
(el precio puede ser `65` o `1.50`).

### Resolver el mapa (automático) — solo si tu balanza no coincide

En `/sniffer`, sección **"Resolver el mapa"**: pon un peso, mira qué marca,
escríbelo, **Capturar**; repite con 3–4 pesos MUY distintos; **Resolver mapa**.

---

## 7. Ficheros

| Fichero | Contenido |
|---|---|
| `peso_balanza_cam.ino` | setup/loop, WiFi, portal cautivo, orquestación |
| `config.h` | **todo lo ajustable**: modo, WiFi, TZ, mapa sniffer |
| `seg7.h` | tabla de 7 segmentos (compartida) |
| `weighlog.h/.cpp` | máquina de estados del pesaje + historial (común) |
| `ocr7seg.h/.cpp` | cámara + análisis de segmentos (solo si `ENABLE_OCR`) |
| `sniffer_tm1640.h/.cpp` | ISR del bus DA/SL + TM1640 (solo si `ENABLE_SNIFFER`) |
| `web_ui.h/.cpp` | servidor web y páginas |

## 8. Notas

- Si la placa se reinicia al conectar la WiFi (brownout), aliméntala por un
  USB-C con buena corriente.
- Con los dos métodos activos, si el bus se decodifica con basura pon
  `OCR_PAUSE_SNIFFER 1` en `config.h`.
- El sniffer es de solo lectura y no perturba el bus.
