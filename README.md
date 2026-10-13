# ATEM tally lučka (ESP32-C6)

Tally lučka za Blackmagic ATEM (preizkušeno z ATEM Mini Extreme) na plošči
**Waveshare ESP32-C6-LCD-1.47** (zaslon ST7789 172×320, RGB LED WS2812).
Lučka se z ATEM-om pogovarja neposredno po UDP protokolu (vrata 9910), brez računalnika.

## Funkcije

- Cel zaslon **rdeč** = kamera v programu, **zelen** = v predogledu, črn = mirovanje.
- Velika številka kamere in ime vhoda iz ATEM-a.
- Zgornja vrstica: ura (NTP, Ljubljana) in moč WiFi signala.
- Spodnja vrstica: kamera v programu, stanje snemanja (REC) in pretakanja (LIVE).
- Osvetlitev zaslona utripa, ko WiFi ali ATEM nista povezana.
- RGB LED na plošči sledi tally stanju (lahko izklopiš).
- Spletna stran na `http://<IP lučke>/` ali `http://tally.local`: ATEM IP, kamera, svetlost,
  rotacija, ime lučke, seznam WiFi omrežij (do 6).
- Več lučk se samo najde (UDP broadcast, vrata 9911); na strani katerekoli lučke urejaš vse.
  Vsaka ima tudi svoje ime `tally-xxxx.local`.
- Če WiFi 30 s ni dosegljiv, se odpre dostopna točka `ATEM-Tally-XXXX`
  (geslo `AP_PASS`), stran na `http://192.168.4.1`.

## Tipka (BOOT)

Na glavnem zaslonu:
- **klik** = naslednja kamera 1..8
- **drži 0,5 s** = meni

V meniju:
- **klik** = naslednja postavka
- **drži** = potrdi
- **dvoklik** = nazaj

Meni: rotacija, svetlost, WiFi omrežje, ATEM IP, RGB LED, velikost številke, ura,
splet/WiFi (vklopi AP), info, ponastavitev.

## Strojna oprema

Samo plošča Waveshare ESP32-C6-LCD-1.47, brez dodatnega ožičenja. Napajanje prek USB-C.
Uporabljeni pini: zaslon MOSI 6, SCLK 7, CS 14, DC 15, RST 21, osvetlitev 22;
RGB LED 8; tipka BOOT 9.

## Prevajanje in nalaganje

1. Kopiraj `secrets.example.h` v `secrets.h` in vpiši svoja WiFi omrežja in geslo AP.
2. Namesti [arduino-cli](https://arduino.github.io/arduino-cli/), ESP32 jedro (3.3.x)
   in knjižnici *Adafruit GFX* ter *Adafruit ST7735 and ST7789*.
3. Prevedi in naloži (zamenjaj COM vrata):

```
arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=huge_app AtemTally
arduino-cli upload  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=huge_app -p COM7 AtemTally
```

Nastavitve se shranijo v NVS in ostanejo po ponovnem nalaganju.

## Serijska konzola (115200)

- `status` izpiše WiFi, IP, ATEM in število najdenih lučk.
- `wifi <ime>` preklopi na znano omrežje.
