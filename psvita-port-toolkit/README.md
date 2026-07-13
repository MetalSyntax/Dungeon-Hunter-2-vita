# PS Vita Port Toolkit (Android / cocos2d-x, arquitectura SoLoader)

Documentación genérica (sin nada específico de un juego puntual), destilada de un port real llevado de cero
a jugable en hardware físico. Pensada para reusar en el próximo port.

## Contenido

- **`PORTING_GUIDE.md`** — guía paso a paso, fase por fase, con un checklist al final. Empezar por acá.
- **`skills/psvita-porting/`** — la Skill de Claude Code con el detalle técnico de cada tema (toolchain,
  carga de `.so`, JNI, input, empaquetado de assets, LiveArea/VPK, debugging en hardware real).

## Cómo usar esto en un proyecto nuevo

Copiar la carpeta `skills/psvita-porting/` a `.claude/skills/psvita-porting/` dentro del repo del nuevo
port (o a `~/.claude/skills/psvita-porting/` para que esté disponible en cualquier proyecto). Claude Code la
va a activar sola cuando el trabajo sea de este tipo. Leer `PORTING_GUIDE.md` como mapa general del proceso.
