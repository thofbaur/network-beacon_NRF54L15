# Bekannte Datenfehler in den Logs

## ID1-Doppelmapping (0-9 <-> 48-57)

In den Kontakt-Logs (`contacts_*.csv`, erzeugt von `postprocessing.py`) ist die
**ID2**-Spalte (der gesehene Beacon) immer korrekt.

Die **ID1**-Spalte (der meldende Beacon) kann fehlerhaft sein - aber nur unter
diesen beiden Bedingungen gleichzeitig:

- Das **Auslesedatum** liegt bis einschließlich **2026-08-15 22:00 Uhr** -
  gemeint ist der Zeitpunkt, zu dem die Log-Zeile selbst aufgezeichnet wurde
  (die rohe Log-Timestamp / `message_timestamp` in `postprocessing.py`, bzw.
  der Dateiname des `.log`-Files), NICHT der aus dem Timer errechnete
  "Contact Local Time"-Zeitstempel in `contacts_*.csv`. Ein Kontakt kann aus
  einem Log ausgelesen worden sein, das vor dem 15.8. 22:00 Uhr aufgezeichnet
  wurde, aber trotzdem einen deutlich früheren oder späteren resolved
  Contact-Zeitstempel tragen (Backlog-Einträge über einen Reboot hinweg, siehe
  `resolve_event_timestamp`). Für die Einordnung des Fehlers zählt das
  Auslesedatum, nicht die Contact-Zeit in der CSV.
- Das Doppelmapping betrifft ausschließlich die Paare aus dem niedrigen
  Bereich **0-9** und dem hohen Bereich **48-57** (0<->48, 1<->49, 2<->50, ...,
  9<->57 - siehe Default-Ranges in `current_timer_zero_report.py`). Eine ID1
  außerhalb dieser beiden Bereiche ist von diesem Fehler nicht betroffen.

D.h.: Nur wenn das zugrundeliegende Log vor dem 2026-08-15 22:00 Uhr
ausgelesen wurde UND ID1 einer der Werte 0-9 oder 48-57 ist, kommt eine
Verwechslung mit dem jeweils gepaarten Wert in Frage - unabhängig davon,
welche Contact-Zeit in der CSV steht. In allen anderen Fällen ist ID1 genauso
vertrauenswürdig wie ID2.
