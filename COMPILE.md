# Fix driver de compilation `./cc` (macOS + Linux) — auto-détection libs C/C++ + multi-job

## Contexte
Je développe un binaire `./cc` qui agit comme un **driver Clang** (j’ai réimplémenté le process du driver en code).
Aujourd’hui, la compilation C++ ne se comporte pas comme attendu sur **macOS** et **Linux**, notamment à cause de la résolution des librairies/headers C/C++ (libstdc++/libc++), et de flags qui ne devraient pas être passés au mauvais sous-outil.

## Objectifs (must-have)
1. **Portabilité**
   - La compilation doit fonctionner sur **macOS** et **Linux**.

2. **Auto-détection des headers/libs C/C++ (pas de hardcode)**
   - Sous Linux et macOS, `./cc` doit **trouver automatiquement** les include paths et libs standards (C et C++) sans chemins codés en dur.

3. **Fidélité au driver Clang**
   - Le comportement du driver doit être **le plus fidèle possible** à celui du driver de Clang (construction de commande, sélection des outils, propagation des flags).

4. **Résolution multi-job**
   - La résolution des libs/headers C/C++ doit être **compatible multi-job** (plusieurs invocations/compilations en parallèle).

5. **Ne pas casser l’existant**
   - Le code actuel qui gère la compilation en **LLVM-IR** ne doit **pas** être cassé (backward compatibility).

## Problème actuel (repro)
Pour compiler du C++ actuellement, ces commandes ne donnent pas le comportement prévu sous Linux et macOS :

### Cas 1
```bash
./cc -x c++ bar.cpp foo.cpp
```

Résultat actuel :
```bash
error: unknown argument: '-fskip-odr-check-in-gmf'
error: unknown argument: '-fskip-odr-check-in-gmf'
```

### Cas 2
```bash
./cc bar.cpp foo.cpp
```

Résultat actuel :
```bash
error: unknown argument: '-fskip-odr-check-in-gmf'
error: unknown argument: '-fskip-odr-check-in-gmf'
```

## Multi-level compilation (tests multi-job)
La compilation à plusieurs niveaux doit être gérée.

### Avec instrumentation
```bash
./cc --instrument bar.cpp foo.cpp
./cc --instrument bar.o foo.o
```

### Sans instrumentation
```bash
./cc bar.cpp foo.cpp
./cc bar.o foo.o
```

## Exigences d’architecture / qualité
- Je veux une solution **propre**, pas un “quick fix”.
- Chaque correction doit être **scalable** et **maintenable**.
- Qualité attendue : séparation claire des responsabilités (par ex. parsing args / détection toolchain / construction commandes / exécution / cache / logs).
- La solution doit être pensée pour évoluer (ajout futur de plateformes, de modes, d’outils, etc.).

## Tests requis (must-have)
- Générer des **scripts de test** pour couvrir **chaque cas** listé ci-dessus.
- Inclure des tests **macOS** + **Linux**.
- Les tests Linux doivent tourner via un **Dockerfile** (fourni/généré).
- Les tests doivent valider :
  - que la compilation passe,
  - que les flags sont routés au bon outil (pas de flag inconnu envoyé au mauvais niveau),
  - que les chemins C/C++ standard sont résolus automatiquement,
  - que le multi-job ne casse pas la résolution (pas de race conditions, pas de cache global non protégé).

## Livrables attendus
1. Proposition d’architecture (modules/composants, responsabilités).
2. Modifications de code nécessaires (approche détaillée, points d’intégration).
3. Stratégie de détection toolchain/libc++/libstdc++ (macOS vs Linux).
4. Suite de tests :
   - scripts macOS,
   - Dockerfile + scripts Linux,
   - commandes exactes exécutées,
   - critères de succès/échec.

## Critères d’acceptation (Definition of Done)
- Les 4 commandes de compilation (cpp + obj, avec/sans `--instrument`) fonctionnent sur macOS et Linux.
- Aucune dépendance à des chemins hardcodés pour les headers/libs C/C++.
- Comportement cohérent avec Clang driver (mêmes grandes décisions : frontend, linker, stdlib sélection, etc.).
- Aucun impact négatif sur le mode LLVM-IR existant.
- Tests automatisés disponibles et exécutables (Linux via Dockerfile).

---

# Implémentation (résumé technique)

## Architecture retenue
- **Parsing args / filtrage** : `extractRuntimeConfig` conserve les flags `--ct-*`, les arguments restants restent intacts.
- **Détection toolchain** : nouveau module `toolchain.cpp` (résolution du binaire clang, resource dir, mode C++).
- **Construction driver** : utilisation du driver Clang pour produire un `JobPlan` (cc1 + autres jobs).
- **Exécution** :
  - **non‑instrumenté (ToFile)** : délègue à `Driver::ExecuteCompilation` (fidélité Clang).
  - **instrumenté / ToMemory** : **cc1** en-process via `Cc1Runner`.
  - **linker** via `Linker` (exécution des jobs non-cc1).

## Changements de code (points d’intégration)
- `src/compilerlib/compiler.cpp`
  - Suppression des include/sysroot hardcodés.
  - Ajout d’un `DriverConfig` (toolchain + resource dir + driver-mode C++).
  - Support du **link-only** pour `--instrument` et sans instrumentation.
- `Cc1Runner` utilisé pour les jobs cc1 en mode instrumenté / ToMemory (diagnostics consolidés).
  - Support des actions `-E` / `-fsyntax-only` (préprocess & syntax-only).
  - `-x none` injecté avant la runtime en mode `--instrument`.
- `src/compilerlib/toolchain.cpp` + `include/compilerlib/toolchain.hpp`
  - Détection robuste de clang + resource dir.
  - Heuristique de mode C++ (sources, `-x`, `-stdlib`, libs, inspection symboles `.o/.a`).
- `CMakeLists.txt`
  - Ajout du module toolchain + dépendances LLVM Object.
  - Propagation de `CT_CLANG_EXECUTABLE` (clang trouvé à la configuration).

## Stratégie de détection toolchain / stdlib
1. **clang path** :
   - `CT_CLANG` (env) →
   - `CT_CLANG_EXECUTABLE` (détecté par CMake) →
   - `clang-${LLVM_VERSION_MAJOR}` →
   - `clang` / `clang++`.
2. **resource dir** :
   - calcul via `clang::driver::Driver::GetResourcesPath(clang_path)` si dispo,
   - sinon fallback `CLANG_RESOURCE_DIR` (CMake).
3. **mode C++** :
   - détecté via `-x c++`, extensions `.cpp/.cc/.cxx/.mm`, `-stdlib=`, `-lstdc++`/`-lc++`,
   - pour link-only, scan rapide des symboles C++ dans les objets/archives.
4. **sysroot macOS** :
   - si `-isysroot/--sysroot` absent, tentative via `xcrun --show-sdk-path`.

## Suite de tests (scripts)
### macOS
Script : `test/scripts/macos_compile.sh`
- `./cc --instrument foo.cpp bar.cpp`
- `./cc --instrument -x c++ -c foo.cpp bar.cpp && ./cc --instrument foo.o bar.o`
- `./cc foo.cpp bar.cpp`
- `./cc -x c++ -c foo.cpp bar.cpp && ./cc foo.o bar.o`

### Linux
Script : `test/scripts/linux_compile.sh`
- mêmes commandes que macOS.

### Docker (Linux)
Dockerfile : `test/docker/Dockerfile`
- installe LLVM/Clang 20,
- build + exécute `test/scripts/linux_compile.sh`.

## Exécution des tests
```
# macOS (après build)
bash test/scripts/macos_compile.sh

# Linux local (après build)
bash test/scripts/linux_compile.sh

# Linux via Docker
docker build -f test/docker/Dockerfile .
```
