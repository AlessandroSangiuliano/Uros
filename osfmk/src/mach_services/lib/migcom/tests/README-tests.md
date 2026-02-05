# Migcom — Descrizione dei test

## Cosa viene testato ✅
- Esecuzione del binario `migcom` con un input minimo (file `.mig`) per verificare che non vada in crash e termini con codice di uscita 0.
- Test di smoke dopo refactor/compilazioni per accertare che la funzionalità di base del tool sia intatta.

## Come lo testo 🔧
- Test automatico registrato in CTest (file: `CMakeLists.txt` del modulo).
- Comando eseguito dal test:

  ```sh
  /path/to/build/src/mach_services/lib/migcom/migcom -V -user /dev/null -server /dev/null -sheader /dev/null -dheader /dev/null < tests/minimal.mig
  ```

- Passi per eseguirlo manualmente:
  1. Configurare il build: `cmake -S . -B build`
  2. Compilare `migcom`: `cmake --build build --target migcom -j N`
  3. Eseguire il test di directory: `ctest --test-dir build/src/mach_services/lib/migcom -V`

## Test inclusi nel modulo 🧪
- `minimal.mig`: test minimale che verifica che `migcom` riconosca un semplice `subsystem` e termini correttamente (smoke test).
- `routine-simple.mig`: verifica il parsing di una `simpleroutine` con `request_port` e un argomento `in`; controlla la validazione del tipo della porta e la correttezza della firma della routine.
- `routine-complex.mig`: esercita parsing di una routine con `request_port`, argomenti `in` e `inout`; verifica la gestione di più categorie di argomenti e la robustezza del parser.
- `skip-only.mig`: verifica che gli statement `skip;`, commenti e whitespace vengano ignorati senza produrre errori.

Tutti i test sopra devono terminare con codice di uscita `0` (esito `Passed` in CTest).

## Perché lo testo (motivo) 💡
- Fornisce un controllo semplice e rapido che il processo principale (parsing, generazione file, e terminazione corretta) non sia rotto dalle modifiche.
- È utile come primo gate di integrazione prima di eseguire test più approfonditi o test di integrazione.

## Risultati attesi 🎯
- Il comando deve terminare con codice di uscita `0` (esito `Passed` in CTest).
- Nessun crash o dump; output di diagnostica (se `-V`) è accettabile, ma non errori fatali.

## Estendere i test ➕
- Aggiungere ulteriori file `.mig` in `tests/` che verificano casi più complessi (argomenti polimorfici, KPD vari, template statici, opzioni diverse del comando).
- Per ogni nuovo test, aggiungere una riga `add_test(...)` nel `CMakeLists.txt` del modulo per registrarlo in CTest.

## Note pratiche e suggerimenti ✨
- Il test attuale reindirizza output/file a `/dev/null` per renderlo deterministico e rapido.
- Per test che confrontano output, si possono generare file temporanei e usare `diff` per verificare le aspettative.

---

Se vuoi, posso ora:
1. Aggiungere test addizionali con casi rappresentativi, oppure
2. Promuovere i test del modulo in modo che vengano eseguiti anche dal `ctest` a livello di progetto (root). 

Dimmi quale opzione preferisci.