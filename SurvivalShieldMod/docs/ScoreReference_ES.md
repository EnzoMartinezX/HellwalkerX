# Referencia de Puntuación y Carga (Hellwalker v1.0)

El sistema **Hellwalker** utiliza una economía de puntos propia e independiente de las medallas de campaña para cargar la embestida potenciada del escudo.

---

## 1. Base de puntos por categoría de demonio

| Categoría | Puntos base | Porcentaje por defecto | Puntos otorgados por defecto |
|---|---:|---:|---:|
| **Demonios menores (`Fodder`)** | `100` | `10 %` | `10` pts |
| **Demonios pesados (`Heavy`)** | `300` | `10 %` | `30` pts |
| **Demonios superpesados (`Superheavy` / `Boss`)** | `1000` | `10 %` | `100` pts |

Todos los porcentajes son configurables desde **`Extras -> Mods -> Hellwalker`** entre `5 %` y `50 %` (en pasos de `5 %`). Internamente el núcleo trabaja con centipuntos (`× 100`) para no perder fracciones.

---

## 2. Reglas de atribución de bajas (sólo bajas reales del Slayer)

Para evitar que muertes ambientales o eventos de menú sumen puntos falsos, `GameplayBridge` valida tres condiciones antes de acreditar una baja:

1. **Ventana de daño del jugador (`PlayerDamageWindow`):** el demonio debe haber recibido daño originado por la entidad del Slayer local o estar dentro de una ventana activa de ataque/parry del jugador.
2. **Verificación de muerte real (`IsAiDead`):** cuando la ruta de respaldo por daño detecta una posible baja letal, comprueba que la vida real de la entidad (`health <= 0.0f` en `+0x301C`) o su función virtual `vtable[+0xED0]` confirmen que el demonio efectivamente murió (impidiendo que un golpe no letal o un parry sumen puntos de muerte).
3. **Deduplicación por entidad (`RecentKillDedup`):** tabla de 256 ranuras con ventana de `120` segundos que garantiza que cada entidad `idAI2*` acredite su baja exactamente una sola vez aunque el motor dispare tanto `OnDamage` como `OnKilled`.

---

## 3. Aporte por bloqueo perfecto (`Parry`)

- Configurable de `0 %` a `50 %` en pasos de `10 %` (`0 %` deshabilita la ganancia por bloqueo).
- Cada bloqueo exitoso con el escudo otorga ese porcentaje directo sobre la **Carga objetivo de embestida** (`targetScore`), sin importar la categoría del demonio atacante:
  - Ejemplo: con carga objetivo `100` y aporte por bloqueo `10 %`, cada parry suma `10` puntos.

---

## 4. Indicador en el HUD

- El contador de **4 dígitos** en el HUD muestra el puntaje acumulado actual (de `0000` a `9999`).
- Mientras la carga está por debajo del objetivo, el indicador permanece en estado de carga; al alcanzar o superar la **Carga objetivo de embestida**, pasa a estado **Listo (rojo)** indicando que el próximo *Shield Bash* será potenciado.
