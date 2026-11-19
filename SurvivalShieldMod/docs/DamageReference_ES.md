# Referencia de Daño de la Embestida Potenciada (Hellwalker v1.0)

Cuando el contador de energía de **Hellwalker** alcanza el `100 %` de la puntuación objetivo, la siguiente embestida de escudo (*Shield Bash*) contra un demonio consume la carga y reemplaza el daño base del impacto por una fórmula escalada según la **vida máxima (`maxHealth`)** y la **categoría** del objetivo (`SurvivalShieldCore::SuperchargedBashDamageFor`).

---

## Escalado de impacto por categoría (`poweredDashDamage = 2000` por defecto)

El parámetro configurable en el menú (**Potencia de impacto de embestida**, de `100` a `5000`, valor por defecto `2000`) actúa como multiplicador de escala (`scale = poweredDashDamage / 2000.0`) sobre los porcentajes base de cada categoría:

| Categoría | Daño de impacto (% de `maxHealth` a escala 2000) | Tope máximo de impacto (% de `maxHealth`) | Piso mínimo de daño | Efecto esperado a escala por defecto (2000) |
|---|---:|---:|---:|---|
| **Demonios menores (`Fodder`)** | `100 %` | `100 %` | `poweredDashDamage` | Baja instantánea en 1 embestida. |
| **Demonios pesados (`Heavy`)** | `35 %` | `75 %` | `poweredDashDamage` | `35 %` de impacto + `24 %` de quemadura = **`59 %` total** (muere en 2 embestidas o 1 embestida + seguimiento). |
| **Demonios superpesados (`Superheavy`)** | `22 %` | `55 %` | `poweredDashDamage` | `22 %` de impacto + `24 %` de quemadura = **`46 %` total** (requiere 2 embestidas + combate con armas). |
| **Jefes / Colosos (`Boss`)** | `12 %` | `35 %` | `poweredDashDamage` | `12 %` de impacto + `24 %` de quemadura = **`36 %` total**. |

---

## Daño base vanilla de referencia (`decls/damage/damage/shield/`)

| Ataque vanilla | Daño declarado | Archivo `.decl` |
|---|---:|---|
| Embestida de escudo (*Shield Bash*) | `100` fijo | `bash_direct.decl` |
| Embestida en sprint | `400 – 800` | `bash_sprint_direct.decl` |
| Carga Juggernaut | `800` fijo | `juggernaut_charge_direct.decl` |
