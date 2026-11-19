# Estructura Técnica — SurvivalShieldMod (Hellwalker)

Esta carpeta contiene los componentes del mod **Hellwalker** y del gestor integrado de mods para **DOOM: The Dark Ages (Update 5)**.

Para la guía general de uso e instalación de mods de la comunidad, consultá el [`README.md`](../README.md) principal en la raíz del proyecto.

---

## Organización de carpetas

- [`bin/`](bin/)
  - [`PlaySurvivalShield.bat`](bin/PlaySurvivalShield.bat) — Punto de entrada para iniciar el juego con Hellwalker y la biblioteca de mods.
  - [`hellwalker_session.ps1`](bin/hellwalker_session.ps1) — Lanzador transaccional que gestiona el respaldo/restauración vanilla, el entorno de *staging* de mods activos y la caché en `%LOCALAPPDATA%\DOOMTheDarkAges\SurvivalShield\`.
  - [`generate_mods_menu.ps1`](bin/generate_mods_menu.ps1) — Escanea [`mods/`](mods/), normaliza carpetas de terceros, sincroniza el estado activado/desactivado (`.off`) y genera dinámicamente las páginas `.decl` de **`Extras -> Mods`**.
  - [`apply_intro_skip.ps1`](bin/apply_intro_skip.ps1) — Sincroniza el estado del mod de omisión de introducción (`SkipIntro`) con `com_skipIntroVideo` en `DOOMTheDarkAgesConfig.local`.
- [`mods/`](mods/)
  - Carpeta donde se colocan los paquetes de mods.
  - [`mods/Hellwalker/`](mods/Hellwalker/) — Paquete base de interfaz, textos localizados y menú de configuración de Hellwalker.
  - [`mods/SkipIntro/`](mods/SkipIntro/) — Paquete incluido para omitir los videos de introducción al abrir el juego.
- [`assets/`](assets/)
  - [`assets/static/decls/`](assets/static/decls/) — Plantillas `.decl` base para el botón **`Mods`** en **`Extras`** y la página de ajustes de Hellwalker.
  - [`assets/fonts/`](assets/fonts/) y [`assets/hellwalker_icon.ico`](assets/hellwalker_icon.ico) — Recursos gráficos del HUD y del acceso directo.
- [`native/`](native/)
  - Código fuente C++20 de `SurvivalShield.dll`, `SurvivalShieldLoader.exe` y `SurvivalShieldTests.exe`.
  - [`native/tools/patch_u5.py`](native/tools/patch_u5.py) — Generador local del ejecutable en caché con los parches de carga de recursos y los *thunks* de Update 5.
- [`docs/`](docs/)
  - Documentación de referencia sobre las fórmulas de puntuación, escalado de daño por categoría y motor de quemadura.
