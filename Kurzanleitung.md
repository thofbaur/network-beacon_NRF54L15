# Grundlagen
- Installation der nRF Connect App auf dem Smartphone
- Installation einer IDE mit nRF Connect Extension (empfohlen: Visual Studio Code) -> installation des zephyr SDK (nrf leitet einen durch)
- Klonen des Repo https://github.com/thofbaur/network-beacon_NRF54L15
- Installation von Extraputty (oder vergleichbarem seriellem Logger; Wichtig, er muss mit Zeitstempel loggen; Putty alleine kann das nicht)
# Basistest:
- In VS Code öffnen von Network_Beacon_nrf54
- In der nRF Connect Extension im Bereich "Application" hinzufügen einer Build Configuration -> bei Board Target  nrf54l15dk/nrf54l15/cpuapp/ns  für das Development kit auswählen (eventuell bei Optimization level für Debugging auswählen)
- Development Kit anschließen und einschalten
- Unter Actions: "Flash"
- Das Board sollte alle 20s ganz kurz auf LED0 leuchten
- In der nRF Connect app sollte ein Device namens "DSA" auftauchen mit Nutzlast: <0x00FF> 0xC0
	FF steht für die ID, da die Adresse des DK noch nicht im Code enthalten ist, wird hier der Default FF gewählt. (du kannst die Adresse ergänzen, s.u.)
	C ist der Indikator für die Anzahl gespeicherter Kontakte. (Hier nicht 0, da eine Testfunktion in main.c den Speicher initial mit 100 Einträgen füllt)
# Erweiterter Test:
- Wie Basis nur als Board Target nrf54l15tag/nrf54l15/cpuapp/ns auswählen und vor dem flashen das DB auf den Debug Stecker des Development Kit stecken
- Wieder Flash und das Tag sollte alle 20s kurz blau blinken und mit Namen DSA erscheinen.
Kombitest:
- Tag wie oben
- Für ein DK den Ordner Network_Base_nrf54 öffnen
- Build Konfiguration für das DK erstellen.
- Putty (oder vergleichbaren Seriellen Leser) mit dem DK verbinden. Der COMport kann in Visual Studio unter connected Devices abgelesen werden. Hier sind 2 angegeben, einer ist für die sichere Partition, der andere der relevante. Geschwindigkeit 112500 
- Dann flashen. Es sollte u.a. eine Meldung erscheinen "Network Base ready. Press button0 to scan
- Button0 am DK drücken. in der Konsole sollten die übermittelten Daten erscheinen.

# Mögliche ToDos:
- Erfassen aller Adressen
  Möglichkeit 1:
  - Ablesen aller Addressen von den Tags (unter der Batterie)
  Möglichkeit 2:
  - Aktivierung des Logs in der nRF Connect App (Smartphone oder Notebook) und Anschalten aller Beacons. Sie sollten erkannt werden und die Adresse geloggt. Ein Filter auf den Namen "Nordic LBS" schafft Klarheit
  Möglichkeit 3:
  - Umschreiben der Network_Base, so dass sie Devices mit dem Namen "Nordic LBS" verbindet. Dabei wird auf die Adresse auf die Konsole geschrieben (dafür muss Putty loggen)
  WICHTIG: Die so ermittelte Adresse muss in umgekehrter Reihenfolge in radio_ids.c geschrieben werden.
- Stromverbrauch:
  - Tag flashen und eingeschaltet lassen. Wie lange lebt es mit dem Code?
- Aufzeichnen verschiedener Abstände und mit Hindernissen -> Vergleich der RSSI
- Programmierung einer Python App die direkt auf dem Notebook läuft, sich verbindet und die Daten speichert.

# Impementierung ToDos:
- Spannungsmessung
- Beschleunigungsmessung
- Optional: Entfernungsbestimmung
- Optimierung: 
  - Maximierung NUS Transfer
  - Minimierung geschriebene Zeichen in Log
  - Auflösen aller ToDos im Code
	


