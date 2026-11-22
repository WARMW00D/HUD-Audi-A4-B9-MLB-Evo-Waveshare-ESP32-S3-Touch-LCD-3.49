# CAN Log Analysis: Lane Marking Type, Engine Data, ACC and Side Assist

Comparison of two platforms using real sniffer logs against factory/open DBC matrices.

**Data sources:**

| Platform | Logs | Matrix |
|---|---|---|
| MLB-Evo (Audi A4 B9) | `E:\temp\logs\2026-09-28 … 2026-11-21` (~139 IDs on the bus) | `E:\temp\MLBevo_Gen2_MLBevo_ICAN_KMatrix_V8.21.01F_20210129_EICR.dbc` (factory I-CAN K-Matrix, 561 messages / 4511 signals) |
| MQB (Skoda Octavia A7) | `E:\temp\logs_mqb\2026-10-24_MQB_Octavia` (4,270,599 frames, 122 IDs) | `E:\temp\vw_mqb.dbc` (commaai/opendbc, 113 messages) + HUD project documentation |
| MQB (VW Passat) | `E:\temp\logs_mqb\2026-10-29_MQB_Passat` (1,894,435 frames, 153 IDs) | same |

Analysis date: 2026-11-23.

---

## 1. Lane Marking Type from Camera (Solid / Dashed)

### Conclusion: the signal does not exist on either platform

### 1.1. MLB-Evo (I-CAN K-Matrix)

The DBC was checked for every naming variant: `solid/dashed`, `durchgezogen/gestrichelt/unterbrochen`,
`Linientyp/Linienart/Linienerkennung`, `Fahrbahnmarkierung`, `Markierungstyp`, `Spurmarkierung`,
messages `LDW/HCA/AWV/MFK/Kamera` — **no signal carrying the line type**.

Closest matches in the matrix:

- `STP_04` (0x29A), signals `STP_FBM_links_ZA` (22|1) and `STP_FBM_rechts_ZA` (27|1) —
  just 1 bit "lane marking on/off" for the traffic jam assist supplementary display
  (`VAL_`: 0 = `keine_Anzeige_oder_init`, 1 = `FBM_an`). No line type,
  and the 0x29A frame itself is **absent** from the logs (0 frames).
- The camera frame `LDW_02` (0x397) is absent from the I-CAN DBC as a message, but is
  present in the logs (~1,009,300 frames). Layout per MQB opendbc (see below): only statuses,
  warnings and Lernmodus. Empirical byte analysis across all logs:

  | Byte | Distinct values | Set (start) | Interpretation |
  |---|---|---|---|
  | 0 | 1 | `00` | — |
  | 1 | 3 | `00 40 80` | status bits (bits 8–15: Gong/SW_Warnung) |
  | 2 | 2 | `00 10` | LDW_Texte (bits 16–19) |
  | 3 | 1 | `00` | — |
  | 4 | 5 | `00 50 60 90 A0` | `LDW_Lernmodus_rechts` 36\|2 + `LDW_Lernmodus_links` 38\|2 |
  | 5 | 215 | `24…37` | `LDW_DLC` 40\|8 (lateral distance to line, ×0.01 m) |
  | 6 | 64 | `00…13` | `LDW_TLC` 48\|5 (time to line crossing, ×0.1 s) |
  | 7 | 1 | `00` | bits 56–63: warnings, LED (almost always 0) |

  There is no field resembling a "line type" in the frame.

### 1.2. MQB (opendbc vw_mqb.dbc)

A search across the whole file (113 messages) for `Spur|Markierung|Linientyp|Linienart|solid|dashed`
among signals — **zero matches**. The camera (MFK) only publishes `LDW_02` (0x397):

```
BO_ 919 LDW_02: 8 XXX
 SG_ LDW_Gong                    : 12|2
 SG_ LDW_SW_Warnung_links/rechts : 14|1, 15|1
 SG_ LDW_Texte                   : 16|4
 SG_ LDW_Lernmodus               : 21|3
 SG_ LDW_Lernmodus_rechts        : 36|2   (0–3)
 SG_ LDW_Lernmodus_links         : 38|2   (0–3)
 SG_ LDW_DLC                     : 40|8 (0.01, -1.25) [m]  ← lateral distance to line
 SG_ LDW_TLC                     : 48|5 (0.1) [s]        ← time to line crossing
 SG_ LDW_Warnung_links/rechts    : 56|1, 57|1
 SG_ LDW_Status_LED_gelb/gruen   : 61|1, 62|1
```

Empirical data from MQB logs (byte 4 = Lernmodus values):

| Car | byte4 values | Decoding |
|---|---|---|
| Octavia | `00 50 60 90 A0 B0 D0 E0 F0` (9 values) | combinations of Lernmodus L/R = 0–3 |
| Passat | `00 50 A0 F0` | same |

Lernmodus semantics (per openpilot + verified by the HUD project):
`0` = no line/off, `1` = line not recognized (yellow), `2` = line seen and held
(green), `3` = observed on the Octavia — probably a transitional/intervention state.

**Bottom line:** the camera only publishes "line seen / being held / warning" plus
(in the MQB layout) the distance to the line `LDW_DLC`. The marking type (solid/dashed)
is not put on the bus on either MLB-Evo or MQB.

---

## 2. Engine Data: RPM, Oil/Coolant Temperature, Boost

### 2.1. Layouts per the matrices

| Parameter | MLB-Evo (I-CAN) | MQB (opendbc) | Match |
|---|---|---|---|
| Engine RPM | `Motor_12` 0x0A8, `MO_Drehzahl_01` 48\|16 ×0.25 rpm | same layout | yes |
| Cluster RPM | `Motor_04` 0x107, `MO_Anzeigedrehz` 24\|12 ×3 rpm | same layout | yes |
| Oil temperature | `Motor_07` 0x640, `MO_Oel_Temp` 16\|8, (1, −60) °C | same | yes |
| Coolant temperature | `Motor_07` 0x640, `MO_Kuehlmittel_Temp` 24\|8, (0.75, −48) °C | same | yes |
| Relative boost | `Motor_04` 0x107, `MO_rel_Ladedruck` 0\|8 ×0.4 % (DBC comment: "für Boostanzeige") | not described in opendbc, but byte 0 is alive (same position) | effectively yes |
| Absolute boost | `Motor_04` 0x107, `MO_Ladedruck` 39\|9 ×0.01 bar | same layout | yes |
| Max boost (gauge scale) | `Motor_18` 0x670, `MO_max_Ladedruck` 0\|6 ×0.1 bar | — | — |
| Oil pressure | `Motor_04` 0x107, `MO_Oeldruck` 16\|8 ×0.04 bar | same layout | yes |
| Temperature QBits | `Motor_07`: `MO_QBit_Oel_Temp` 1\|1, `MO_QBit_Kuehlmittel_Temp` 2\|1 | same | yes |

### 2.2. What the logs show

| Parameter | A4 (MLB-Evo) | Octavia (MQB) | Passat (MQB) |
|---|---|---|---|
| `0x0A8` Motor_12 (true RPM ×0.25) | **present**, alive: ~860–3234 rpm (41,130 frames in sample) | **frame absent from the bus** | **frame absent from the bus** |
| `0x107` MO_Anzeigedrehz ×3 | alive (e.g. 861 rpm at idle) | **alive**, 0–5844 rpm (199,032 frames) | **alive**, 0–2424 rpm (70,385 frames, calm log) |
| Oil temp (0x640) | alive, typically 83–91 °C; `194` invalid marker occurs | alive, 57–93 °C | alive; `194` marker occurs |
| Coolant temp (0x640) | alive, up to ~90.8 °C | alive, 72.8–89.2 °C | alive, up to 88.5 °C |
| Boost % (byte 0 in 0x107) | **dead: always 0** (95,873 frames in a row) | **alive**, 100 distinct values, ~0–80 % | **alive**, 148 distinct values, ~0–100 % |
| Boost, bar (39\|9 in 0x107) | **dead: always 0** | **alive**, up to ~2.3 bar (226 distinct raw) | **alive**, up to ~2.2 bar |
| `0x670` MO_max_Ladedruck | **dead: always 0** (byte 0 = `00` in all frames) | — | — |
| Oil pressure (16\|8 in 0x107) | **dead: always 0** | constant `248` (0xF8 — invalid) | constant `254` (0xFE — invalid) |

Real frame examples (A4, 2026-11-21):

```
0A8: 65 E7 DF D4 92 64 88 0D  -> RPM = 0x0D88 * 0.25 = 866 rpm, QBit=0
640: 00 46 93 B4 7B 60 5B 3D  -> Oel = 0x93-60 = 87°C, Kuehl = 0xB4*0.75-48 = 87.0°C, IntakeAir = 4.5°C
107: 00 00 00 1F 01 00 67 C6  -> relLade=0%, Ladedruck=0 bar, Oeldruck=0 bar, AnzeigeDrehz=861 rpm
```

**Why boost is zero on the A4:** in the I-CAN DBC, `MO_rel_Ladedruck`/`MO_Ladedruck`
have the attribute `GenSigMissingSourceValue = 0` (when the source is missing the gateway
sends 0; a `Fehlerwert` would be 255/511). I.e. the A4's engine ECU does not send these
signals and the gateway relays zeros. On the MQB cars the source exists — values are alive.

**Conclusions for the HUD:**

- RPM: on MLB — `MO_Drehzahl_01` (0x0A8, 0.25 resolution); on MQB — only
  `MO_Anzeigedrehz` (0x107, 3 rpm resolution).
- Oil and coolant temperatures work everywhere, identical layout (0x640).
- Boost: on MQB — yes (% and bar from 0x107); on the A4 — not available in principle.
- Oil pressure — unavailable anywhere.

---

## 3. ACC: Distance to the Vehicle Ahead

### 3.1. MLB-Evo (A4) — distance EXISTS, in meters

**`ACC_14` (0x2A8)** — the "distance warning" (Abstandswarner) frame:

```
BO_ 680 ACC_14: 8 Gateway
 SG_ ACC_Status_Anzeige             : 16|3     (0=off, 1=passive/init, 2=active ...)
 SG_ ACC_Geschw_Zielfahrzeug        : 22|10 (0.32) [km/h]  ← target vehicle speed
 SG_ ACC_Zeitluecke_Abstandswarner  : 48|6  (0.1) [s]      ← time gap to target
 SG_ ACC_Abstand_Abstandswarner     : 54|9  (1)   [m]      ← DISTANCE IN METERS (511 = no target)
```

Log facts (all dates):

- `ACC_Status_Anzeige`: mostly 1/2; live data when =2.
- Examples while actively following:

  | Frame (hex) | Abstand | Zeitluecke | Target V |
  |---|---|---|---|
  | `00 00 02 41 00 00 CA 09` | 39 m | 1.0 s | 83.2 km/h |
  | `00 00 42 41 00 00 4A 0A` | 40 m | 1.0 s | 83.2 km/h |
  | `00 00 C2 39 00 00 4A 0A` | 41 m | 1.0 s | ~74 km/h |

  (39–42 m at ~83 km/h ≈ 1.6–1.8 s — consistent with real following;
  the Zeitluecke in the frame is the warning threshold, not the actual gap.)
- When no target: `Abstand = 511`, `Zeitluecke = 63` (invalid).

**`ACC_12` (0x2A6)** — the main ACC frame:

- `ACC_Abstandsindex_02` 22|10 — dimensionless radar distance index
  (`VAL_`: 0 = init/passive, 1022 = `aus_passiv`, 1023 = `aktiv_Freifahrt`).
- Facts: 1023 — 1,702,710 frames (free road), 1022 — 43,979;
  with a target — values like 383–392.
- Plus `ACC_Wunschgeschw_02` 12|10 ×0.32, `ACC_Tempolimit` 0|5,
  `ACC_Gesetzte_Zeitluecke` 37|3, `ACC_Relevantes_Objekt_02` 45|2.

**`ACC_16` (0x266)** — predictive ACC (PACC) only: `ACC_Fahrprogramm_PACC`,
`ACC_Offset_Default_PACC`, `ACC_vLimit1_PACC` etc. No distance.

### 3.2. MQB — only an index; meters are switched off

**`ACC_02` (0x30C)** — the main ACC frame on MQB (present on both cars):

```
BO_ 780 ACC_02: 8 XXX
 SG_ ACC_Wunschgeschw_02   : 12|10 (0.32) [km/h]  (1023 = not set)
 SG_ ACC_Abstandsindex     : 24|10 [1..1021]      ← radar distance index
 SG_ ACC_Gesetzte_Zeitluecke : 37|3               (1..5 levels)
 SG_ ACC_Relevantes_Objekt : 46|2
 SG_ ACC_Status_Anzeige    : 61|3
```

`VAL_ 780 ACC_Status_Anzeige`: 0=`Hauptschalter_aus`, 1=`init`, 2=`passiv`,
3=`aktiv`, 4=`im_Hintergrund_uebertreten`, 5=`Abschaltreaktion`, 6/7=`aus`.

Log facts:

| Parameter | Octavia | Passat |
|---|---|---|
| Set speed | 30–100 km/h (34,194 frames with value) | 30–70 km/h (10,251 frames) |
| Status | 2: 50,370, 3: 11,081, 4: 798, 7: 3,636 | 2: 17,833, 3: 2,623, 0: 898 |
| Abstandsindex at status=3/4 | mean 332–429, max 972, zeros 12–18 % | mean 520–562, max 972, zeros 0 % |
| Abstandsindex at status=1/6/7 (off) | always 0 | always 0 |

The index strictly correlates with ACC activity: off — always 0;
active with a target — non-zero (grows on approach, arbitrary scale 0–1021).

**`ACC_04` (0x324)** — the counterpart of MLB `ACC_14` (distance in meters):

```
BO_ 804 ACC_04: 8 XXX
 SG_ ACC_Zeitluecke_Abstandswarner : 36|6 (0.1) [s]
 SG_ ACC_Abstand_Abstandswarner    : 42|9 (1)   [m]
```

**On both MQB cars every frame contains zeros** (Octavia: 65,956 frames = 0;
Passat: 21,527 frames = 0) — the Abstandswarner display is not activated/configured.
There is effectively no distance in meters on MQB.

**`ACC_10` (0x117)** — present only on the Passat (64,581 frames); this is AWV/ANB (pre-crash):
`AWV_Vorstufe`, `ANB_Zielbrems_*`, `PCF_Time_to_collision` (56|8 ×0.01 s).
No target distance (there is TTC to an obstacle, but that is pre-crash, not ACC).

### 3.3. Side Assist — flags only everywhere

`SWA_01` (0x30F) on both platforms carries only states:

```
 SG_ SWA_Infostufe_SWA_li / SWA_Warnung_SWA_li : 26|1, 27|1
 SG_ SWA_Infostufe_SWA_re / SWA_Warnung_SWA_re : 42|1, 43|1
 SG_ SWA_Sta_aktiv / SWA_Sta_passiv            : 19|1, 20|1
 SG_ SWA_Blindheit_erkannt, SWA_rel_Fehler, ASW_Status/Modus ...
```

**There is no distance to the vehicle in the blind spot in either matrix** — only
"object present + warning level (info/warning)".

Log facts (info-level toggling is visible on all three cars):

| Car | Typical stats (inf_li, warn_li, inf_re, warn_re, aktiv, passiv) |
|---|---|
| A4 | `(0,0,0,0,0,1)` 204,260; `(0,0,0,0,1,0)` 169,710; `(1,0,0,0,1,0)` 10,276 — left info level |
| Octavia | `(0,0,0,0,0,1)` 44,610; `(1,0,0,0,1,0)` 755; `(0,0,1,0,1,0)` 430 |
| Passat | `(0,0,0,0,0,1)` 18,531; `(1,0,0,0,1,0)` 1,144; `(0,0,1,0,1,0)` 757 |

---

## 4. Summary Table for the HUD

| Data | A4 (MLB-Evo) | Octavia A7 (MQB) | Passat (MQB) |
|---|---|---|---|
| Marking type (solid/dashed) | ❌ not in matrix or on bus | ❌ no | ❌ no |
| Lane state (seen/held/warning) | ✅ LDW_02 0x397 | ✅ LDW_02 0x397 | ✅ LDW_02 0x397 |
| Distance to line (DLC) / time (TLC) | ✅ 0x397: 40\|8, 48\|5 | ✅ same | ✅ same |
| RPM | ✅ 0x0A8 ×0.25 | ✅ 0x107 ×3 (coarser) | ✅ 0x107 ×3 |
| Oil temp | ✅ 0x640 | ✅ 0x640 | ✅ 0x640 |
| Coolant temp | ✅ 0x640 | ✅ 0x640 | ✅ 0x640 |
| Boost bar/% | ❌ zeros (no source) | ✅ 0x107 | ✅ 0x107 |
| ACC set speed | ✅ 0x2A6 | ✅ 0x30C | ✅ 0x30C |
| ACC status | ✅ 0x2A8 (16\|3) | ✅ 0x30C (61\|3) | ✅ 0x30C (61\|3) |
| ACC distance in meters | ✅ 0x2A8 (54\|9) + time (48\|6) + target V (22\|10) | ❌ 0x324 zeroed | ❌ 0x324 zeroed |
| ACC distance index | ✅ 0x2A6 (22\|10) | ✅ 0x30C (24\|10) | ✅ 0x30C (24\|10) |
| Side Assist | flags 0x30F | flags 0x30F | flags 0x30F |
| SWA distance | ❌ | ❌ | ❌ |

### Practical implications for the MQB HUD port

1. **A boost gauge is possible on MQB** (not on the A4): `0x107` byte 0 ×0.4 % and/or
   39\|9 ×0.01 bar. For a "max boost" scale, `MO_max_Ladedruck` (0x670) has not been
   checked on MQB — worth a separate look.
2. **ACC distance on MQB is only a relative scale** via `ACC_Abstandsindex`
   (0x30C, 24\|10): draw "closer/farther" without meters. If meters are really needed —
   options: calibrate the index against known distances, or enable the Abstandswarner
   in the cluster coding (then 0x324 will presumably come alive).
3. **RPM on MQB must come from 0x107** (`MO_Anzeigedrehz`, 3 rpm step) —
   `Motor_12` (0x0A8) does not reach the sniffer's tap point.
4. Marking type — unavailable anywhere; at most "line seen/held" + DLC/TLC.

---

## 5. Engine Parameters Available Live (Full DBC × Log Scan)

Method: automatic parsing of both DBCs (all `Motor_*`, `OBD_*`, `TSK_*`, `WIV_*` messages),
intersection with IDs actually seen in the logs, and per-signal distinct-value statistics
("alive" = >2 distinct values; "const" = a single value across all frames).
Housekeeping fields (CRC/COUNTER) excluded. Analysis date: 2026-11-23.

### 5.1. A4 (MLB-Evo) — live parameters

**RPM and load**

| Signal | Frame | Bits | Scale | Log facts |
|---|---|---|---|---|
| `MO_Drehzahl_01` | 0x0A8 Motor_12 | 48\|16 | ×0.25 rpm | alive, up to 3815 (n=1.54M) |
| `MO_Momentenintegral_02` | 0x0A8 | 40\|7 | % | alive, 0–100 % (load) |
| `MO_Anzeigedrehz` | 0x107 Motor_04 | 24\|12 | ×3 rpm | alive (coarser duplicate) |
| `MO_obere_Drehzahlgrenze` | 0x670 Motor_18 | 56\|8 | ×50 | constant 4750 |

**Torque, Nm** (`MO_Faktor_Momente_02`=1 in 0x641 → raw value = Nm)

| Signal | Frame | Bits | Facts |
|---|---|---|---|
| `MO_Mom_Ist_Summe` | 0x0A7 Motor_11 | 22\|10, −509 | alive, −509…+431 |
| `MO_Mom_Soll_Roh` | 0x0A7 | 12\|10, −509 | alive, −63…+509 |
| `MO_Mom_Soll_gefiltert` | 0x0A7 | 42\|10, −509 | alive |
| `MO_Mom_Schub` | 0x0A7 | 52\|9, −509 | alive, −103…−10 |
| `MO_Mom_Traegheit_Summe` | 0x0A7 | 32\|10, −509 | alive, −202…+124 |
| `MO_Mom_Begr_stat` | 0x0A8 | 21\|9 | alive, 40…509 |
| `MO_Mom_Begr_dyn` | 0x0A8 | 30\|10, −509 | alive |
| `MO_Mom_Begr_Schalt` | 0x154 Motor_28 | 22\|10, −509 | alive |
| `MO_Drehzahl_VM` | 0x154 | 48\|16 ×0.25 | alive, up to 3686 |

Power is computed as: P[kW] = M[Nm] × n[rpm] / 9549.
The intended "ready-made" signals `MO_Anzeige_Leistung/Drehmoment` (0x147 Motor_29)
and `MO_Anzeige_Leistung_Max/Drehmoment_Max` (0x56F Motor_25) are **dead** (zeros).

**Temperatures**

| Signal | Frame | Bits | Scale | Facts |
|---|---|---|---|---|
| `MO_Oel_Temp` | 0x640 Motor_07 | 16\|8 | −60 | alive, 21–93 °C (194 = invalid) |
| `MO_Kuehlmittel_Temp` | 0x640 | 24\|8 | ×0.75 −48 | alive, 21–93 °C |
| `MO_Ansaugluft_Temp` | 0x640 | 8\|8 | ×0.75 −48 | alive, −24.75…+58.5 (142.5 = invalid) |
| `MO_ITM_Kuehlmittel_Temp` | 0x647 Motor_09 | 0\|8 | ×0.75 −48 | alive, 20–90 °C |
| `OBD_Eng_Cool_Temp` | 0x391 OBD_01 | 8\|8 | −40 | alive, 21–93 °C |

**Fuel / consumption**

| Signal | Frame | Bits | Facts |
|---|---|---|---|
| `MO_KVS` (injection, µl/stroke) | 0x107 | 48\|15 | alive, 0–32767 → instant consumption |
| `MO_CO2_Faktor` | 0x647 | 44\|12 | dead (0) |
| All `SCR_*` (AdBlue) | 0x647 | — | dead |

**Oil**

| Signal | Frame | Bits | Facts |
|---|---|---|---|
| `WIV_Oelstand` | 0x3C7 Motor_26 | 16\|4 ×12.5 % | **alive, 0–100 %** |
| `WIV_Oelmenge` | 0x3C7 | 43\|5 ×125 ml | constant 1000 |
| `WIV_Oelsystem_aktiv` | 0x3C7 | 24\|1 | =1 |
| `MO_Oeldruck` | 0x107 | 16\|8 ×0.04 bar | dead (0) |

**Pedal, acceleration, mass, slope**

| Signal | Frame | Bits | Facts |
|---|---|---|---|
| `MO_Fahrpedalrohwert_01` | 0x121 Motor_20 | 12\|8 ×0.4 % | alive, 0–100 % |
| `MO_Fahrpedalgradient` | 0x121 | 21\|8 ×25 %/s | alive |
| `MO_Solldrehz_Leerlauf` | 0x121 | 40\|8 ×10 | alive, 860–1200 |
| `TSK_a_Soll_gradientenbegrenzt` | 0x121 | 57\|7 ×0.1 −7.2 m/s² | alive, −2.8…+5.5 |
| `TSK_Fahrzeugmasse_02` | 0x65F Motor_16 | 48\|8 ×32 kg | alive, 1600–1792 kg |
| `TSK_Steigung_02` | 0x65F | 56\|8 ×0.8 −101.6 % | alive, −101.6…+102.4 % |

**Statuses and misc (alive)**

- 0x3BE Motor_14: `MO_StartStopp_Status` (12\|2), `MO_Gangposition` (20\|4),
  `MO_Motor_laeuft` (39\|1), `MO_StartVorauss_erfuellt`, `MO_Fahrer_bremst`, `MO_Kl_75/50`.
- 0x640: `MO_Heizungspumpenansteuerung` (53\|4 ×10 %), `MO_Nachlaufzeit_Heizungspumpe`,
  `MO_Hoeheninfo` (barometric pressure, 0.95–1.01 rel.).
- 0x670 Motor_18: `MO_Abstellzeit` (32\|8 ×8 min — parking time, alive).
- 0x641 Motor_Code_01 (static): `MO_Anzahl_Zyl`=4, `MO_Hubraum`=2.0 L,
  `MO_Leistung`=140 kW, `MO_DPF_verbaut`=1, `MO_Code`=12, `MO_Getriebe_Code`=5.

**Dead on the A4 (do not display):** `MO_rel_Ladedruck`, `MO_Ladedruck`, `MO_max_Ladedruck`,
`MO_Oeldruck`, `MO_Istgang`/`MO_Sollgang` (0x107), `MO_E85_Sensor`, all of Motor_27 (0x0BF),
`MO_Hochschaltdrehz` and `MO_Schaltblitz_Anz` (0x147), `MO_Zylinderabschaltung` (0x3BE),
`MO_Anzahl_Abgesch_Zyl` (0x670), `MO_EPCL`, TSK_vMax* (0x2C2).

### 5.2. MQB (Octavia / Passat) — live parameters

**RPM and boost**

| Signal | Frame | Bits | Octavia | Passat |
|---|---|---|---|---|
| `MO_Anzeigedrehz` | 0x107 | 24\|12 ×3 | alive, up to 5844 | alive, up to 2424 |
| `MO_Ladedruck` | 0x107 | 39\|9 ×0.01 bar | **alive, 0–2.6 bar** | **alive, 0–2.24 bar** |
| relative boost (byte 0, ×0.4 %; not in opendbc) | 0x107 | 0\|8 | **alive, ~0–80 %** | **alive, ~0–100 %** |
| `MO_Drehzahl_01` | 0x0A8 | 48\|16 ×0.25 | frame absent | frame absent |
| `MO_obere_Drehzahlgrenze` | 0x670 | 56\|8 ×50 | const 6800 | const 4850 |

**OBD parameters (0x391 OBD_01) — noticeably richer on MQB than on the A4**

| Signal | Bits | Scale | Octavia | Passat |
|---|---|---|---|---|
| `OBD_Calc_Load_Val` | 0\|8 | ×0.392 % | alive, 0–45 % | alive, 0–100 % |
| `OBD_Eng_Cool_Temp` | 8\|8 | −40 °C | alive, 69–86 | alive, 11–89 |
| `OBD_Abs_Throttle_Pos` | 16\|8 | ×0.392 % | alive, 9.8–86 % | alive, 0–88 % |
| `OBD_Abs_Load_Val` | 24\|16 | ×0.392 % | alive, 0–236 % | dead (const) |
| `OBD_Abs_Pedal_Pos` | 40\|8 | ×0.392 % | alive, 14–80 % | alive, 0–48 % |

**Torque, pedal (native frames)**

| Signal | Frame | Octavia | Passat |
|---|---|---|---|
| `Motor_11` torques (Nm) | 0x0A7 | frame absent | **alive** (−56…+513 raw) |
| `MO_Fahrpedalrohwert_01` | 0x121 | frame absent | alive, 0–57 % |
| `MO_Solldrehz_Leerlauf` | 0x121 | — | alive, 750–940 |
| `TSK_a_Soll_gradientenbegrenzt` | 0x121 | — | alive, −2.1…+5.5 m/s² |

(Octavia: pedal/throttle/load are covered via OBD_01.)

**Temperatures** — 0x640, alive on both: oil 57–93 (Octavia) / 11–93 (Passat, 194=invalid),
coolant 72.8–89.2 / 11.2–88.5, intake 24–58.5 / −48…31.5; `MO_ITM_Kuehlmittel_Temp` (0x647) — alive on both.

**Fuel**

| Signal | Frame | Octavia | Passat |
|---|---|---|---|
| `MO_KVS` (µl/stroke) | 0x107 48\|15 | alive → consumption | alive → consumption |
| `OBD_TG_Sens_Rohwert_1` (level sender, Ohm) | 0x65E 16\|12 ×0.5 | alive, 76–278 Ohm | alive, 0–251 Ohm |
| `OBD_TG_Sens_Rohwert_2` | 0x65E 28\|12 | dead | alive, 0–274 Ohm |

**Oil wear and service interval (WIV_01 0x642 — MQB only)**

| Signal | Bits | Scale | Octavia | Passat |
|---|---|---|---|---|
| `WIV_Verschleissindex` | 0\|16 | ×2e-8 | alive (up to 1.46e-5) | alive |
| `WIV_Russindex` (soot) | 16\|16 | ×2e-8 | dead | alive |
| `WIV_t_min/t_max` | 32\|6, 40\|6 | months | 12/24 | 24/24 |
| `WIV_W_min/W_max` | 48\|7, 56\|7 | ×1000 km | 17000/31000 | 17000/31000 |

**Oil level** — `WIV_Oelstand` (0x3C7, 16\|4 ×12.5 %): weakly alive
(Octavia 0–12.5 %, Passat 0–50 %); `WIV_Oelmenge` 0–1000 ml.

**Mass/slope (0x65F):** `TSK_Fahrzeugmasse_02` alive (Octavia 1376–1568 kg,
Passat up to 1664), `TSK_Steigung` alive.

**Statuses (0x3BE):** Start/Stop status alive on both, `MO_Motor_laeuft`,
`MO_Status_Zylabschalt_01`, `MO_Fahrer_bremst`.

**Coding (0x641, static):**

- Octavia: `MO_Anzahl_Zyl`=4, `MO_Hubraum`=1.8 L, `MO_Leistung`=132 kW,
  `MO_Kraftstoffart`=1 (petrol), `MO_Code`=66, `MO_Getriebe_Code`=18.
- Passat: 4 cyl, 2.0 L, 140 kW, `MO_DPF_verbaut`=1, `MO_Code`=12, `MO_Getriebe_Code`=24.

**Dead on MQB:** `MO_Istgang/Sollgang`, `MO_Oeldruck` (constants 248/254 = invalid),
`SCR_*` (0x647), `MO_CO2_Faktor`, `MO_E85_Sensor`, `TSK_Wunschgeschw` on the Octavia
(almost always 327 = "not set"), some lamps/texts in 0x3C7.

### 5.3. Summary: HUD "Engine Screen"

| Parameter | A4 (MLB) | Octavia (MQB) | Passat (MQB) | Source |
|---|---|---|---|---|
| RPM | ✅ ×0.25 | ✅ ×3 | ✅ ×3 | 0x0A8 / 0x107 |
| Torque, Nm | ✅ | ❌ | ✅ | 0x0A7 |
| Power (M×n/9549) | ✅ | ❌ | ✅ | computed |
| Load, % | ✅ 0x0A8 40\|7 | ✅ | ✅ | 0x0A8 / OBD_01 |
| Boost, bar / % | ❌ | ✅ | ✅ | 0x107 |
| Oil temp | ✅ | ✅ | ✅ | 0x640 16\|8 |
| Coolant temp | ✅ | ✅ | ✅ | 0x640 24\|8 (+ 0x647, 0x391) |
| Intake air temp | ✅ | ✅ | ✅ | 0x640 8\|8 |
| Pedal, % | ✅ | ✅ (OBD) | ✅ | 0x121 / OBD_01 |
| Throttle, % | ❌ | ✅ | ✅ | OBD_01 16\|8 |
| Instant consumption | ✅ | ✅ | ✅ | 0x107 MO_KVS |
| Oil level, % | ✅ | ~ | ~ | 0x3C7 WIV_Oelstand |
| Oil pressure | ❌ | ❌ | ❌ | — |
| Oil wear / service | ❌ | ✅ | ✅ | 0x642 WIV_01 |
| Fuel level (raw) | ❌ | ✅ | ✅ | 0x65E |
| Vehicle mass / slope | ✅ | ✅ | ✅ | 0x65F |
| Start/Stop, "engine running" | ✅ | ✅ | ✅ | 0x3BE |
| Barometric pressure | ✅ | ~ | ~ | 0x640 MO_Hoeheninfo |

Notes:

1. **Torque in Nm on the A4 and Passat** allows drawing live power; on the Octavia,
   boost + load from OBD_01 can be shown instead of torque.
2. **`MO_KVS`** (µl of fuel per power stroke) is present on all three — instant
   consumption is computed as: Q[L/h] = KVS[µl] × (n/2)[cycles/min] × N_cyl × 60 / 10⁹
   for a four-stroke engine.
3. Invalid-value markers: oil temp `194`, intake temp `142.5`,
   oil pressure `248/254`, `SCR_Reichweite` `32767`, KVS `32767` (overflow,
   there is a `MO_KVS_Ueberlauf` flag).
