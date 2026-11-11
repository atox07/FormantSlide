FormantSlide - generatore nativo FL Studio

First slice MIDI note assegna il primo slice a un tasto (default 60, C5 in FL).
Con root 60, C5 usa lo slice 1, C#5 lo slice 2 e D5 lo slice 3.
Ogni slice nella zona mappata suona alla velocita e al pitch di registrazione.
Fuori dalla zona viene trasposto lo slice piu vicino, come nel VST3.

Usa i colori 1 e 2 per le vocali. Per uno slide FL disegna una slide note
dello stesso colore sovrapposta alla vocale. Per il sistema personalizzato
usa una nota normale sovrapposta sul colore 5 o sui colori 3-16, secondo la
modalita selezionata. La durata proviene da FL, dal MIDI importato se il dato
manca, oppure da Fallback glide. Uno slide alla nota di partenza riporta la
voce al pitch originale. La vocale e lo slice devono durare fino allo slide.

I vecchi progetti usano la root salvata (60 nelle versioni con rilevamento
automatico) corretta per la precedente calibrazione. Se la tua chromatic
iniziava altrove, imposta First slice MIDI note. I tagli manuali si salvano
con il progetto.

Build Windows: usa il contenuto di questa directory come radice del branch
flnative. Avvia il workflow Build FormantSlide FL Native (Windows) in Actions.
Chiudi FL prima di sostituire FormantSlide_x64.dll con la build risultante.
