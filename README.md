# Hellwalker & Mod Library — DOOM: The Dark Ages (Update 5)

**Hellwalker** es un proyecto para **DOOM: The Dark Ages (Steam Update 5)** con dos objetivos principales:

1. **Mecánica de combate Hellwalker (Escudo de Supervivencia):** añade un sistema de carga ofensiva para el escudo alimentado por las bajas del Slayer y los bloqueos perfectos (*parrys*), que transforma la embestida del escudo en un impacto crítico capaz de incendiar a los demonios.
2. **Biblioteca y Gestor Integrado de Mods (`Extras -> Mods`):** incorpora un apartado nativo dentro del menú del juego para que cualquier jugador pueda organizar, activar (`Sí`) o desactivar (`No`) mods propios o de otros creadores sin tener que mover ni borrar archivos manualmente antes de cada partida.

---

## 1. Biblioteca y Gestor de Mods de la Comunidad

El proyecto está pensado para funcionar como base y repositorio local de mods de terceros:

### ¿Cómo agregar un mod externo?
1. Copiá o descomprimí la carpeta del mod dentro de [`SurvivalShieldMod/mods/`](SurvivalShieldMod/mods/).
2. **El nombre que le pongas a la carpeta será el nombre que verás dentro del juego.**
   - Ejemplo: si llamás a la carpeta `SkipIntro`, en el menú **`Extras -> Mods -> Mods`** aparecerá como **`SkipIntro`**.
   - Si el mod incluye un archivo `darkagesmod.txt` con el nombre original del paquete, autor o versión, esos datos se mostrarán automáticamente en el panel de descripción lateral al seleccionar el mod.
   - Si descargaste un mod que solo trae la carpeta `decls/` (sin `darkagesmod.txt`) o quedó metido dentro de una subcarpeta al descomprimir el `.zip`, el lanzador lo normaliza y genera el manifiesto automáticamente.

### ¿Cómo activar o desactivar mods desde el juego?
- Entrá a **`Extras -> Mods`** y cambiá a la pestaña **`Mods`**.
- Elegí **`Sí`** (habilitado) o **`No`** (deshabilitado) en cada mod de tu lista.
- La preferencia queda guardada en `%LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\mods_menu_state.json`.
- En el siguiente inicio, el lanzador renombra automáticamente los mods desactivados a `.off` y empaqueta **únicamente** los mods activos.

---

## 2. Mecánica de Combate Hellwalker

Desde **`Extras -> Mods -> Hellwalker`** podés configurar en tiempo real:

- **Hellwalker (`Sí` / `No`):** habilita o deshabilita la mecánica durante la partida.
- **Carga objetivo de embestida:** puntuación necesaria para cargar la embestida potenciada del escudo (mostrada en el contador de 4 dígitos del HUD).
- **Aporte por baja (Demonios menores / pesados / superpesados):** porcentaje de puntos que otorga cada categoría cuando la baja la realiza el Slayer (`100` base en menores, `300` en pesados, `1000` en superpesados y jefes).
- **Aporte por bloqueo (`Parry`):** porcentaje de la carga objetivo que suma cada bloqueo exitoso con el escudo.
- **Embestida potenciada + Quemadura:**
  - Al llegar al `100 %` de la carga objetivo (`300` puntos por defecto), el próximo *Shield Bash* consume la carga e inflige daño crítico escalado según la vida máxima y categoría del demonio (`100 %` en menores, `35 %` en pesados, `22 %` en superpesados, `12 %` en jefes).
  - Si el demonio sobrevive al impacto, queda envuelto en llamas con el efecto visual nativo del motor y recibe **8 ticks** de quemadura (`3 %` de su vida máxima por tick, `24 %` adicional en `4` segundos).

---

## 3. Instalación limpia y segura (100 % Vanilla en reposo)

- **Sin archivos propietarios en el repositorio:** este repositorio no contiene ejecutables ni archivos `.resources` originales del juego.
- **Lanzador transaccional ([`PlaySurvivalShield.bat`](SurvivalShieldMod/bin/PlaySurvivalShield.bat)):**
  - Antes de abrir el juego, respalda los archivos base de Steam y monta los mods activos en una sesión temporal.
  - Al cerrar el juego (o ante cualquier cierre inesperado), restaura automáticamente la carpeta de `DOOMTheDarkAges` a su estado original 100 % vanilla.
  - Todas las cachés generadas (`exe-cache`, `atlan-cache`, `session-backup`) se almacenan fuera del repositorio en `%LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\`.

---

## 4. Requisitos y uso rápido

### Requisitos
- **DOOM: The Dark Ages** para PC (Steam Update 5).
- **Windows 10/11 x64** y **Python 3.10+**.
- **`AtlanModLoader.exe`** ([EntityAtlan](https://github.com/FlavorfulGecko5/EntityAtlan/releases)) copiado en `%LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\tools\AtlanModLoader.exe`.

### Para iniciar el juego con Hellwalker y tu biblioteca de mods
Ejecutá [`SurvivalShieldMod\bin\PlaySurvivalShield.bat`](SurvivalShieldMod/bin/PlaySurvivalShield.bat).

### Compilar desde el código fuente (opcional, para desarrolladores)
```powershell
cmake -S SurvivalShieldMod/native -B SurvivalShieldMod/native/build
cmake --build SurvivalShieldMod/native/build --config Release --target SurvivalShield SurvivalShieldLoader SurvivalShieldTests
ctest --test-dir SurvivalShieldMod/native/build -C Release --output-on-failure
Copy-Item SurvivalShieldMod/native/build/Release/SurvivalShield.dll SurvivalShieldMod/bin/ -Force
Copy-Item SurvivalShieldMod/native/build/Release/SurvivalShieldLoader.exe SurvivalShieldMod/bin/ -Force
```

---

## 5. Autor y Licencia

- **Autor:** **HellwalkerX**
- **Licencia:** Distribuido bajo la licencia [**Apache License 2.0**](LICENSE) (ver también [`NOTICE`](NOTICE)).

