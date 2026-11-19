# Referencia del Efecto de Quemadura (Hellwalker v1.0)

Si un demonio recibe el impacto de una **embestida potenciada** de Hellwalker y **sobrevive al golpe inicial**, el motor nativo (`BurnEngine` + `GameplayBridge`) le aplica un estado de quemadura progresiva junto con el efecto visual de fuego en tiempo real.

---

## 1. Parámetros del daño por quemadura (`BurnEngine.hpp`)

| Parámetro | Valor en v1.0 | Descripción |
|---|---:|---|
| `kTickDamagePct` | `3 %` (`0.03`) | Porcentaje de la vida máxima (`maxHealth`) del demonio descontado en cada tick. |
| `kMaxTicks` | `8` ticks | Cantidad de pulsos de daño por aplicación. |
| `kTickIntervalMs` | `500 ms` | Intervalo entre cada pulso (`8 × 500 ms = 4.0 s` de duración total). |
| **Daño total acumulado** | **`24 %` de `maxHealth`** | Repartido a lo largo de `4` segundos (`3 % × 8`). |
| Piso por tick | `1.0` HP | El daño por quemadura deja al demonio como mínimo en `1.0` HP (`noDeath = true`, fiel al comportamiento del DOT incendiario del motor). |

Si el jugador conecta una segunda embestida potenciada sobre un demonio que ya se está quemando, la quemadura se **refresca** de nuevo a `8` ticks restantes sin duplicar instancias sobre la misma entidad.

---

## 2. Efecto visual de fuego en el modelo del demonio

Al activarse o refrescarse la quemadura sobre un enemigo vivo, `GameplayBridge` invoca dos sistemas visuales nativos del motor sobre la entidad (`idAI2`):

1. **Sistema `LogicFX` (`StartLogicFX`):** inicia el efecto de estado `status_burn_shotgun` / `super_shotgun_secondary_dragons_breath_dot` sobre el componente visual del demonio.
2. **Condición `FX_MAN_ON_FIRE` (`719` start / `722` stop):** activa las partículas de llamas adheridas al cuerpo del demonio (el mismo efecto visual que producen la modificación incendiaria de la escopeta y el flagelo) y las detiene automáticamente al cumplirse los `8` ticks o si el demonio muere antes.
