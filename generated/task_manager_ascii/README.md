Task Manager ASCII

Contenido:
- `task_manager_ascii.js`: app JS para el firmware Bruce

Instalacion sugerida en SD:
1. Crear una carpeta en la SD, por ejemplo:
   `/BruceJS/task_manager_ascii/`
2. Copiar `task_manager_ascii.js` dentro de esa carpeta.
3. Ejecutarla desde el interprete/scripts del firmware.

Persistencia:
- La app guarda sus archivos en la misma carpeta del script.
- Archivos generados por la app:
  - `task_state.jsonl`
  - `task_history.log`

Funciones:
- Crear tareas
- Ver tareas activas y completadas
- Editar titulo
- Definir hora de inicio
- Definir hora de termino
- Marcar completada / reabrir
- Eliminar tareas
- Historial de acciones

Formato:
- Interfaz simple en modo ASCII
- Horas recomendadas: `HHMM` o `HH:MM`
