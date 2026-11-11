FormantSlide Native v0.5 — Auto MIDI anchor + editable slice boundaries
==================================================================

MODIFICHE RISPETTO ALLA V0.4
- Rimosso il controllo First slice MIDI note.
- La PRIMA nota vocale riprodotta dopo il caricamento del WAV diventa il riferimento MIDI del primo slice. Il primo slice suona al pitch originale, senza trasposizione da una root fissata arbitrariamente.
- Gli slice successivi vengono associati ai semitoni successivi del Piano Roll. Non e' un rilevamento automatico della frequenza fondamentale dentro il WAV: e' un ancoraggio MIDI automatico.
- Se il rilevamento del silenzio produce un unico segmento in un WAV continuo di almeno 2 secondi, viene provata una divisione uguale con il numero impostato (default 12). E' una stima: controlla i confini sulla waveform.
- I confini interni degli slice ora sono trascinabili col mouse: trascina le piccole maniglie azzurre sulla forma d'onda. I confini non si possono sovrapporre.
- Salvataggio e ripristino delle posizioni dei tagli personalizzati nel progetto FL Studio.
- Editor nativo: gestione HWND rafforzata e handle azzerato alla chiusura.
- Restano import MIDI, slide per colori, FL native slides e Signalsmith Stretch.

FILE MODIFICATI
  Source/FormantSlideNative.h
  Source/FormantSlideNative.cpp
  Source/WinEditor.cpp
  Source/WaveLoader.cpp
  CMakeLists.txt (soltanto numero versione)

INSTALLAZIONE SUL BRANCH flnative
1. Copia i 4 file .cpp/.h modificati nelle rispettive posizioni di Source/.
2. Sostituisci CMakeLists.txt se vuoi aggiornare anche il numero di versione (non obbligatorio).
3. NON cancellare SDK/, NON sostituire il workflow YAML, NON cancellare altri sorgenti.
4. Commit + Push origin; in Actions fai Run workflow selezionando flnative.
5. Chiudi FL Studio prima di sostituire FormantSlide_x64.dll.

LIMITI IMPORTANTI
- Il primo campione non viene analizzato con un pitch detector: il primo tasto vocale stabilisce l'ancora MIDI. Se parti da un'altra nota, la mappatura cambia; ricarica il WAV per azzerarla.
- Le divisioni di emergenza in parti uguali non sono necessariamente corrette per una chromatic con note di lunghezza diversa: regola manualmente le maniglie.
- L'errore 'Invalid editor window handle' non e' verificabile in questo ambiente; il fix va provato sul tuo FL Studio.
- Nessuna compilazione MSVC/FL Studio e' stata eseguita localmente; la build GitHub Actions resta il controllo effettivo.
