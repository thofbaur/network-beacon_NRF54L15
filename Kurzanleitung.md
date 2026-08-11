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
- Button0 am DK drücken. in der Konsole sollten die übermittelten Daten erscheinen

# Parameter setzen (Network_Control_nrf54)
- Für ein weiteres DK den Ordner Network_Control_nrf54 öffnen
- Build Konfiguration wie beim Basistest (Board Target nrf54l15dk/nrf54l15/cpuapp/ns)
- In src/main.c im Array mfg_data die gewünschte Parameterzeile einkommentieren und den Wert anpassen (mehrere Zeilen gleichzeitig möglich)
- Flashen
- Button 3 am DK startet das Aussenden des Befehls (Name "DSZ"), Button 4 stoppt es wieder
- WICHTIG: Die Adresse dieses DK muss ebenfalls in radio_ids.c im Tag/Beacon-Code eingetragen sein (wie bei den Tags, s.o.), sonst wird der Befehl von den Tags ignoriert, ohne dass irgendeine Meldung erscheint (Accept-List-Filter beim Scannen)
