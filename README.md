# RXT. TNK-1 — Monitor de nivel para tinaco

<p align="center">
  <img src="docs/fotos/tnk1-gabinete.jpg" alt="RXT. TNK-1 en su gabinete" width="600">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/hardware-CERN--OHL--S%20v2-blue" alt="Licencia de hardware">
  <img src="https://img.shields.io/badge/firmware-GPL--3.0-blue" alt="Licencia de firmware">
  <img src="https://img.shields.io/badge/estado-prototipo-orange" alt="Estado">
</p>

Monitor de nivel de agua para tinacos de azotea. Mide el nivel con un sensor ultrasónico a prueba de agua, publica el dato en la nube cada 20 s y te avisa por WhatsApp cuando el tinaco se llena o está por vaciarse. También puedes preguntarle a Alexa cuánta agua hay. No hace falta instalar ninguna app nueva.

Proyecto de hardware abierto de **REXTRONIX.**, hecho en Morelia, Michoacán, con piezas que se consiguen localmente.

---

## El problema

En muchas colonias de Morelia el agua llega por tandeo: se junta en la cisterna y una bomba la sube al tinaco. El tinaco no tiene ningún indicador, así que para saber cuándo apagar la bomba alguien tiene que subir a la azotea a revisar. Si la bomba se queda prendida de más, el agua se tira por el rebosadero; si se apaga antes de tiempo, te quedas sin agua hasta el siguiente tandeo.

## Cómo funciona

```mermaid
flowchart LR
    S["Sensor AJ-SR04M<br/>(ultrasónico)"] -- UART --> M["XIAO ESP32-C6"]
    P["HLK-PM03<br/>127 V CA → 3.3 V"] -- 3.3 V --> M
    M -- "Wi-Fi 2.4 GHz" --> T["ThingSpeak"]
    T --> A["Skill de Alexa<br/>consulta por voz"]
    M -- "Whatabot" --> W["WhatsApp<br/>avisos lleno / casi vacío"]
```

1. El sensor mide el tiempo que tarda el eco en regresar desde la superficie del agua: `d = v · t / 2`.
2. El XIAO ESP32-C6 convierte la distancia a porcentaje:
   `nivel (%) = 100 · (d_vacío − d) / (d_vacío − d_lleno)`
3. Cada 20 s el nivel se publica en un canal de ThingSpeak (`field1`, en %).
4. Alexa lee el último dato del canal cuando le preguntas.
5. El propio ESP32 manda un WhatsApp (Whatabot) al pasar de 97 % («Tinaco lleno») o bajar de 20 % («Tinaco casi vacío»). Cada aviso se manda una vez y se rearma cuando el nivel regresa 5 %.

## Hardware

| Componente | Función |
|---|---|
| AJ-SR04M (modo 5, UART) | Sensor ultrasónico a prueba de agua |
| Seeed XIAO ESP32-C6 | Microcontrolador con Wi-Fi |
| Hi-Link HLK-PM03 | Fuente aislada 127 V CA → 3.3 V |
| Fusible lento T315 mA | Protección contra sobrecorriente |
| Varistor TVR10241 | Protección contra picos de voltaje |
| C1 100 µF + C2 100 nF | Desacoplo del riel de 3.3 V |
| Gabinete ABS Steren 85 × 85 × 50 mm | Gabinete para exterior, con prensaestopa |

El esquemático completo está en [`hardware/`](hardware/).

<p align="center">
  <img src="docs/fotos/esquematico.png" alt="Esquemático de la etapa de potencia" width="700">
</p>

## Firmware

- Dispara una lectura del sensor (`0x01` por UART, 9600 baud) cada 0.5 s.
- Filtro de mediana de 7 muestras para eliminar ecos falsos y el oleaje del agua.
- Zona ciega: a menos de ~22 cm el sensor no puede medir; eso se toma como 100 % (tinaco lleno) en vez de descartarse.
- Si no hay una lectura válida en 10 s, no envía nada.
- Watchdog de 30 s: si el programa se cuelga, el ESP32 se reinicia solo.
- Reconexión automática a Wi-Fi.
- LED de estado: 1 parpadeo = envío correcto, 3 = falló.

### Compilar

1. Instala el [Arduino IDE](https://www.arduino.cc/en/software) y el paquete de placas **esp32** de Espressif.
2. Selecciona la placa **XIAO_ESP32C6**.
3. En **Herramientas › USB CDC On Boot** elige **Enabled** (para ver el monitor serie por USB).
4. Copia `firmware/tinaco_xiao/secrets.example.h` como `secrets.h` en la misma carpeta y llena tu red Wi-Fi (2.4 GHz), la Write API Key de ThingSpeak y tu API key y teléfono de Whatabot. `secrets.h` está en `.gitignore`: no lo subas.
5. Ajusta `TANK_EMPTY_CM` y `TANK_FULL_CM` a tu tinaco (ver abajo) y sube el código.

### Calibración

Mide con un flexómetro, desde el transductor:

- `TANK_EMPTY_CM` (d_vacío): distancia al fondo del tinaco.
- `TANK_FULL_CM` (d_lleno): distancia al nivel máximo de agua. Debe ser **de al menos 20 cm** (zona ciega del sensor).

## ⚠️ Seguridad

Este equipo se conecta a la red eléctrica de 127 V. Arma y prueba la etapa de potencia **sin conexión a la red**, no abras el gabinete mientras esté conectado y **nunca conectes el USB y la HLK-PM03 al mismo tiempo**: ambos alimentan el mismo pin de 3.3 V. Si no tienes experiencia con voltaje de red, pide ayuda a alguien que sí la tenga.

## Estado del proyecto

- [x] Medición y filtrado
- [x] Publicación en ThingSpeak
- [x] Consulta por voz con Alexa
- [x] Aviso por WhatsApp
- [x] Fuente de red con fusible y varistor
- [x] Prototipo en placa perforada dentro del gabinete
- [ ] PCB propia
- [ ] Filtro de entrada (capacitor X2 + choke de modo común)
- [ ] Compensación por temperatura

## Fotos

| Prototipo | Interior | Instalado |
|---|---|---|
| ![Prototipo](docs/fotos/prototipo.jpg) | ![Interior](docs/fotos/interior.jpg) | ![Instalado en la azotea](docs/fotos/instalado.jpg) |

## Licencia

- **Hardware** (`hardware/`): [CERN-OHL-S v2](hardware/LICENSE)
- **Firmware** (`firmware/`): [GNU GPL v3.0](firmware/LICENSE)

Puedes usarlo, modificarlo y venderlo. Si distribuyes una versión modificada, también debe ser abierta y bajo las mismas licencias.

## Autor

**Luis Enrique Madrigal Pintor** · REXTRONIX.
Ingeniería Electrónica, Instituto Tecnológico de Morelia
