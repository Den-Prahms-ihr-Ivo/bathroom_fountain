Damit ist die gesamte reine Logik fertig und getestet. Was jetzt kommt, hängt fast nur noch an Hardware. Ich würde in dieser Reihenfolge vorgehen:

## 1. Bestellen (jetzt)

Die Hue Bridge, den Teufel Ultima 20 und die restlichen Teile von der Einkaufsliste. Die Bridge ist am dringendsten, weil von ihr ein offener Punkt abhängt (siehe Schritt 4).

## 2. Montag: Mikrofon und echte Aufnahmen

Das ist das größte verbleibende Risiko. Die Klatsch-Schwellen beruhen bisher nur auf synthetischen Daten.

Recorder-Firmware auf den DevKit flashen. Die habe ich hier nicht bauen können, weil die ESP-Toolchain blockiert war. Also zuerst pio run im Ordner tools/recorder und Fehler melden, falls welche kommen.
Mit capture.py aufnehmen: Doppelklatscher in Ruhe, bei laufender Dusche und bei Musik (vorerst vom Handy). Dazu Störgeräusche wie Tür, Schritte, Zahnputzbecher und Föhn.
clap_lab.py replay und tune mit deinem mic_frontend und clap_detector laufen lassen. Ergebnis sind Trefferquote, Fehlauslöser und vorgeschlagene Settings.

## 3. Firmware in Etappen auf dem DevKit

Jede Etappe ist für sich lauffähig:

mic_task → Detektor → serielle Ausgabe „DOUBLE CLAP“. Das geht ab Montag.
Encoder und Taster über button und volume.
control_task mit mode_controller. Die MOSFET-Ausgänge vorerst als LEDs auf dem Breadboard.
Konsole und NVS über settings.

## 4. Sobald die Bridge da ist

Pairing, Szenen anlegen, Scene Recall. Dabei klären, ob duration wirklich in Millisekunden angegeben wird. Das ist noch unbestätigt.

## 5. Auf dem Louder

TAS5825M initialisieren, microSD, MP3-Decoder → pcm_mix → I2S, Lautstärke über amp_volume. Danach die Verdrahtung: Buck-Wandler einstellen, MOSFET-Module, Pumpe mit Diode und Elko.

## 6. Abnahme

Ein Dauertest über einen Tag und eine Nacht, um Fehlauslöser zu zählen. Außerdem CLAP-10 prüfen, also Klatschen bei Musik vom eigenen Lautsprecher. Erst dann zeigt sich, ob wir noch eine Playback-Referenz brauchen.
