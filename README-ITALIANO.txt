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

Build Windows (x64, MSVC e CMake >= 3.24):
  cmake --fresh -S . -B build -G "Visual Studio 17 2022" -A x64 -DFL_SDK_DIR="C:/percorso/privato/SDK"
  cmake --build build --config Release --target FormantSlideNative --parallel 2

Non pubblicare i sorgenti proprietari dell'SDK Image-Line su GitHub.
.gitignore esclude SDK/ e build/ anche nelle sottocartelle, ma non rimuove
file gia tracciati o copie nella cronologia Git: verificarle prima di pubblicare.
Se l'SDK e gia nella cronologia, aggiungere .gitignore non basta.

GitHub Actions: il workflow manuale compila interamente sui server GitHub
con runs-on: windows-2022. Non serve alcun runner sul proprio PC e non si
usa la variabile repository FL_SDK_DIR.
Se SDK/ e gia presente nel checkout, il workflow lo utilizza direttamente.
Per un repository senza SDK, configura in Settings > Secrets and variables
> Actions un repository secret FL_SDK_ZIP_URL con un URL HTTPS privato
a uno ZIP dell'SDK (contenente anche il suo CMakeLists.txt).
Il runner scarica ed estrae lo ZIP nella propria cartella temporanea.
Non pubblicare lo ZIP: usa un collegamento con accesso limitato, valido
per la durata della build. Il workflow non include l'SDK negli artefatti.
Avvia Actions > Build FormantSlide FL Native (Windows) > Run workflow,
selezionando il branch con questo workflow aggiornato.
Le esecuzioni gia in attesa del runner personale vanno annullate e riavviate.

Distribuisci build/Release/FormantSlide_x64.dll insieme a
build/Release/THIRD_PARTY_LICENSES.txt. Chiudi FL prima di sostituire la DLL.
Non distribuire vecchie DLL o usare vecchie build/cache per questa versione.

stftPitchShift e incluso in third_party/stftPitchShift, con licenza MIT.
Richiede C++20. Il WAV viene letto e diviso in slice senza preanalisi vocale.
Le note ad altezza originale leggono il WAV stereo direttamente.
Il pitch variabile usa il core STFT con preservazione cepstrale dei formanti,
finestre di 1024 campioni e aggiornamenti ogni 64 campioni.
La prima trasposizione prepara brevemente le finestre della voce corrente.
Il lookahead sul WAV evita di attendere audio in ingresso, ma le finestre
possono smussare gli slide molto rapidi. Qualita e CPU vanno provate in FL.

JUCE resta fissato a 8.0.10. Si prevede l'uso secondo l'EULA JUCE 8,
piano Starter, anziche la licenza alternativa AGPLv3. Starter e gratuito
entro i requisiti dell'EULA (limite applicabile 20.000 USD nei 12 mesi
precedenti e requisiti sulle licenze degli sviluppatori).
I due flag splash/usage a 1 sono impostazioni richieste, non una licenza.
L'ammissibilita Starter e i diritti di distribuzione dell'SDK FL devono essere
verificati dal titolare: il solo CMake non puo certificarli.
Riferimenti: https://juce.com/legal/juce-8-licence/
             https://github.com/jurihock/stftPitchShift/blob/main/LICENSE
