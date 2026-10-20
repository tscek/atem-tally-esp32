# ATEM tally lučka (ESP32-C6)

Tally lučka za Blackmagic ATEM (preizkušeno z ATEM Mini Extreme) na plošči
**Waveshare ESP32-C6-LCD-1.47** (zaslon ST7789 172×320, RGB LED WS2812) ali
**Waveshare ESP32-C6-Touch-LCD-1.47** (zaslon JD9853, touch AXS5106L, brez vgrajene RGB LED; zunanji WS2812 lahko priklopiš na IO8)
ali **Waveshare ESP32-S3-LCD-1.47B** (zaslon ST7789 172×320, RGB LED WS2812)
ali **Waveshare ESP32-C6-LCD-1.3** (kvadratni zaslon ST7789 240×240, RGB LED WS2812).
En firmware za vse plošče z ESP32-C6 in en za ESP32-S3; plošča se prepozna ob zagonu.
Plošči ESP32-C6-LCD-1.47 in 1.3 imata enake pine in se ju ne da ločiti, zato ploščo 1.3
izbereš v meniju (Plosca) ali s serijskim ukazom `board ESP32-C6-LCD-1.3`.
Lučka se z ATEM-om pogovarja neposredno po UDP protokolu (vrata 9910), brez računalnika.

## Funkcije

- Cel zaslon **rdeč** = kamera v programu, **zelen** = v predogledu, črn = mirovanje.
- Velika številka kamere in ime vhoda iz ATEM-a.
- Zgornja vrstica: ura (NTP, Ljubljana) in moč WiFi signala.
- Spodnja vrstica: kamera v programu, stanje snemanja (REC) in pretakanja (LIVE).
- Osvetlitev zaslona utripa, ko WiFi ali ATEM nista povezana.
- RGB LED na plošči sledi tally stanju (lahko izklopiš).
- Spletna stran na `http://<IP lučke>/` ali `http://tally.local`: ATEM IP, kamera, svetlost,
  rotacija, ime lučke, WiFi omrežje z močjo signala. Dodano omrežje se samo shrani
  (do 6), izbereš ga pri lučki (Uredi, WiFi).
- Več lučk se samo najde (UDP broadcast, vrata 9911); na strani katerekoli lučke urejaš vse.
  Vsaka ima tudi svoje omrežno ime, narejeno iz imena lučke: "Tally lučka 1" postane
  `tally-lucka-1` (tako jo vidi usmerjevalnik) in `http://tally-lucka-1.local`.
- Posodobitve prek WiFi (OTA) iz GitHub Release; glej spodaj.
- Če WiFi 30 s ni dosegljiv, se odpre dostopna točka `ATEM-Tally-XXXX`
  (geslo `AP_PASS`), stran na `http://192.168.4.1`.

## Tipka (BOOT)

Na glavnem zaslonu:
- **klik** = naslednja kamera 1..8
- **drži 0,5 s** = meni

V meniju:
- **klik** = naslednja postavka
- **drži** = potrdi
- 2 s brez pritiska = nazaj na začetni ekran (urejanje ATEM IP 10 s, info in splet 30 s)

Izbira drugega WiFi omrežja (v meniju, na spletni strani ali s serijskim ukazom `wifi <ime>`)
lučko samodejno ponovno zažene; v meniju se to zgodi, ko se meni zapre.

Meni: rotacija, svetlost, WiFi omrežje, ATEM IP, RGB LED, velikost številke, ura,
splet/WiFi (vklopi AP), preveri posodobitve, info, ponastavitev.

## Strojna oprema

Samo plošča, brez dodatnega ožičenja. Napajanje prek USB-C.
- **ESP32-C6-LCD-1.47:** zaslon MOSI 6, SCLK 7, CS 14, DC 15, RST 21, osvetlitev 22;
  RGB LED 8; tipka BOOT 9.
- **ESP32-C6-Touch-LCD-1.47:** zaslon MOSI 2, SCLK 1, CS 14, DC 15, RST 22, osvetlitev 23;
  touch I2C SDA 18, SCL 19, RST 20 (naslov 0x63); tipka BOOT 9.
  Zunanji WS2812 (DIN) na pin IO8 letvice, 3V3 in GND.
- **ESP32-S3-LCD-1.47B:** zaslon MOSI 45, SCLK 40, CS 42, DC 41, RST 39, osvetlitev 46;
  RGB LED 38; tipka BOOT 0. Prevajaš za `esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`
  (pini se izberejo samodejno po čipu).

Na touch plošči je v meniju in na spletni strani nastavitev **Touch** (privzeto izklopljena).
Ko je vklopljena, dotik zaslona deluje enako kot tipka BOOT (dotik = klik, drži = meni).

## Prevajanje in nalaganje

1. Kopiraj `secrets.example.h` v `secrets.h` in vpiši svoja WiFi omrežja in geslo AP.
2. Namesti [arduino-cli](https://arduino.github.io/arduino-cli/), ESP32 jedro (3.3.x)
   in knjižnici *Adafruit GFX* ter *Adafruit ST7735 and ST7789*.
3. Prevedi in naloži (zamenjaj COM vrata). Za touch ploščo dodaj
   `--build-property "compiler.cpp.extra_flags=-DBOARD_TOUCH"` k ukazu compile.

```
arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs AtemTally
arduino-cli upload  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=min_spiffs -p COM7 AtemTally
```

Nastavitve se shranijo v NVS in ostanejo po ponovnem nalaganju.

## Serijska konzola (115200)

- `status` izpiše WiFi, IP, ATEM in število najdenih lučk.
- `wifi <ime>` preklopi na znano omrežje.
- `ota` preveri posodobitve (kot postavka v meniju), `reboot` ponovno zažene lučko.

## Posodobitve (OTA)

Lučka po zagonu (po 20 s z WiFi) in nato vsakih 12 ur prebere `manifest.json` zadnje
izdaje na GitHubu (HTTPS). Ročno: meni **Preveri posodobitve**.
- Ko je na voljo novejša verzija, se na zaslonu 5 s odšteva ponudba: **klik ali dotik**
  namesti posodobitev, brez odziva se prekliče (ponovno čez 12 ur ali ročno).
- Ponudbe ni, dokler je lučka na **PROGRAM**; pokaže se, ko gre s programa.
- Med prenosom je na zaslonu napredek. Slika se pred zamenjavo preveri (MD5 iz manifesta).
- Nova verzija mora v 3 minutah dobiti WiFi, sicer se lučka vrne na prejšnjo (rollback).

OTA potrebuje particijsko shemo `min_spiffs` (dve reži po 1,9 MB). Lučko, ki ima še
`huge_app` (FW 2.1 ali starejša), je treba enkrat naložiti prek USB.

Nova izdaja: povečaj `FW_VERSION` v `AtemTally.ino`, nato `./release.sh` (Git Bash).
Skripta prevede obe plošči in objavi GitHub Release `v<verzija>` z
`AtemTally-<plošča>.bin` in `manifest.json`. Repozitorij mora biti javen.
