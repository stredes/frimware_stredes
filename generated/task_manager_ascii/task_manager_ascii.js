"use strict";

var dialog = require("dialog");
var menu = require("menu");
var storage = require("storage");

var FS_KIND = "sd";
var APP_DIR = __dirpath || "/BruceJS/task_manager_ascii";
var STATE_FILE = APP_DIR + "/task_state.jsonl";
var HISTORY_FILE = APP_DIR + "/task_history.log";

function nowStamp() {
  try {
    var d = new Date(Date.now());
    function pad(n) { return n < 10 ? "0" + n : "" + n; }
    return (
      d.getFullYear() + "-" +
      pad(d.getMonth() + 1) + "-" +
      pad(d.getDate()) + " " +
      pad(d.getHours()) + ":" +
      pad(d.getMinutes()) + ":" +
      pad(d.getSeconds())
    );
  } catch (e) {
    return "ts:" + String(now());
  }
}

function trim(str) {
  if (str === null || str === undefined) return "";
  return String(str).replace(/^\s+|\s+$/g, "");
}

function isCanceled(value) {
  return value === null || value === undefined || value === "\x1B";
}

function ensureDir() {
  try {
    storage.mkdir({ fs: FS_KIND, path: APP_DIR });
    return true;
  } catch (e) {
    return false;
  }
}

function defaultState() {
  return {
    version: 1,
    nextId: 1,
    tasks: []
  };
}

function cloneState(state) {
  return JSON.parse(JSON.stringify(state));
}

function appendFile(path, text) {
  ensureDir();
  return storage.write({ fs: FS_KIND, path: path }, text, "append");
}

function readFileOrEmpty(path) {
  try {
    var data = storage.read({ fs: FS_KIND, path: path });
    return data ? String(data) : "";
  } catch (e) {
    return "";
  }
}

function loadState() {
  var raw = readFileOrEmpty(STATE_FILE);
  if (!raw) return defaultState();

  var lines = raw.split("\n");
  var lastLine = "";
  var i;
  for (i = lines.length - 1; i >= 0; i--) {
    if (trim(lines[i]) !== "") {
      lastLine = lines[i];
      break;
    }
  }

  if (!lastLine) return defaultState();

  try {
    var parsed = JSON.parse(lastLine);
    if (!parsed.tasks || typeof parsed.nextId !== "number") return defaultState();
    return parsed;
  } catch (e) {
    return defaultState();
  }
}

function saveState(state, reason) {
  state.savedAt = nowStamp();
  appendFile(STATE_FILE, JSON.stringify(state) + "\n");
  if (reason) appendHistory(reason);
}

function appendHistory(line) {
  appendFile(HISTORY_FILE, "[" + nowStamp() + "] " + line + "\n");
}

function shortTitle(text, maxLen) {
  var t = trim(text);
  if (t.length <= maxLen) return t;
  return t.substring(0, maxLen - 3) + "...";
}

function normalizeTimeInput(value) {
  var raw = trim(value);
  if (raw === "") return "";
  raw = raw.replace(/[^0-9]/g, "");
  if (raw.length === 3) raw = "0" + raw;
  if (raw.length !== 4) return null;

  var hh = parseInt(raw.substring(0, 2), 10);
  var mm = parseInt(raw.substring(2, 4), 10);

  if (isNaN(hh) || isNaN(mm)) return null;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return null;

  return raw.substring(0, 2) + ":" + raw.substring(2, 4);
}

function promptText(title, maxLen, hint) {
  var value = prompt(title, maxLen, hint || "");
  if (isCanceled(value)) return null;
  return trim(value);
}

function promptTime(title, currentValue) {
  while (true) {
    var hint = currentValue
      ? "Actual: " + currentValue + " | HHMM o HH:MM | vacio=limpiar"
      : "HHMM o HH:MM | vacio=sin hora";
    var value = prompt(title, 5, hint);
    if (isCanceled(value)) return null;

    var normalized = normalizeTimeInput(value);
    if (normalized === null) {
      dialog.error("Hora invalida. Usa HHMM o HH:MM", true);
      continue;
    }
    return normalized;
  }
}

function findTaskIndexById(state, id) {
  var i;
  for (i = 0; i < state.tasks.length; i++) {
    if (state.tasks[i].id === id) return i;
  }
  return -1;
}

function taskStatus(task) {
  return task.done ? "[x]" : "[ ]";
}

function taskSchedule(task) {
  var start = task.startTime || "--:--";
  var end = task.endTime || "--:--";
  return start + " -> " + end;
}

function formatTaskLine(task) {
  var idPart = task.id < 10 ? "0" + task.id : "" + task.id;
  return taskStatus(task) + " " + idPart + " " + shortTitle(task.title, 18);
}

function renderTask(task) {
  var lines = [];
  lines.push("TASK #" + task.id);
  lines.push("");
  lines.push("TITLE  : " + task.title);
  lines.push("STATUS : " + (task.done ? "DONE" : "OPEN"));
  lines.push("START  : " + (task.startTime || "--:--"));
  lines.push("END    : " + (task.endTime || "--:--"));
  lines.push("CREATED: " + (task.createdAt || "-"));
  lines.push("UPDATED: " + (task.updatedAt || "-"));
  lines.push("DONE AT: " + (task.completedAt || "-"));
  return lines.join("\n");
}

function renderSummary(state) {
  var openCount = 0;
  var doneCount = 0;
  var i;
  for (i = 0; i < state.tasks.length; i++) {
    if (state.tasks[i].done) doneCount++;
    else openCount++;
  }

  var lines = [];
  lines.push("TASK MANAGER ASCII");
  lines.push("");
  lines.push("TOTAL : " + state.tasks.length);
  lines.push("OPEN  : " + openCount);
  lines.push("DONE  : " + doneCount);
  lines.push("");
  lines.push("DATA DIR:");
  lines.push(APP_DIR);
  return lines.join("\n");
}

function createTask(state) {
  var title = promptText("Nueva tarea", 32, "Titulo");
  if (title === null) return;
  if (title === "") {
    dialog.warning("Titulo vacio", true);
    return;
  }

  var startTime = promptTime("Hora inicio", "");
  if (startTime === null) return;

  var task = {
    id: state.nextId,
    title: title,
    startTime: startTime,
    endTime: "",
    done: false,
    createdAt: nowStamp(),
    updatedAt: nowStamp(),
    completedAt: ""
  };

  state.nextId += 1;
  state.tasks.push(task);
  saveState(state, "CREATE #" + task.id + " " + task.title);
  dialog.success("Tarea creada", true);
}

function editTitle(state, index) {
  var task = state.tasks[index];
  var title = promptText("Editar titulo", 32, "Actual: " + shortTitle(task.title, 20));
  if (title === null) return;
  if (title === "") {
    dialog.warning("Titulo vacio", true);
    return;
  }
  var before = task.title;
  task.title = title;
  task.updatedAt = nowStamp();
  saveState(state, "EDIT TITLE #" + task.id + " " + before + " -> " + title);
}

function editStartTime(state, index) {
  var task = state.tasks[index];
  var startTime = promptTime("Hora inicio", task.startTime);
  if (startTime === null) return;
  task.startTime = startTime;
  task.updatedAt = nowStamp();
  saveState(state, "EDIT START #" + task.id + " -> " + (startTime || "--:--"));
}

function editEndTime(state, index) {
  var task = state.tasks[index];
  var endTime = promptTime("Hora termino", task.endTime);
  if (endTime === null) return;
  task.endTime = endTime;
  task.updatedAt = nowStamp();
  saveState(state, "EDIT END #" + task.id + " -> " + (endTime || "--:--"));
}

function markDone(state, index) {
  var task = state.tasks[index];
  if (!task.endTime) {
    var endTime = promptTime("Hora termino", "");
    if (endTime === null) return;
    task.endTime = endTime;
  }
  task.done = true;
  task.completedAt = nowStamp();
  task.updatedAt = nowStamp();
  saveState(state, "COMPLETE #" + task.id + " " + task.title);
}

function reopenTask(state, index) {
  var task = state.tasks[index];
  task.done = false;
  task.completedAt = "";
  task.updatedAt = nowStamp();
  saveState(state, "REOPEN #" + task.id + " " + task.title);
}

function deleteTask(state, index) {
  var task = state.tasks[index];
  var choice = dialog.choice(["No", "Si, borrar"]);
  if (choice !== "Si, borrar") return;
  appendHistory("DELETE #" + task.id + " " + task.title);
  state.tasks.splice(index, 1);
  saveState(state, "DELETE SNAPSHOT #" + task.id);
}

function taskDetailMenu(state, id) {
  while (true) {
    var index = findTaskIndexById(state, id);
    if (index < 0) return;

    var task = state.tasks[index];
    var actions = [
      "Ver detalle",
      "Editar titulo",
      "Hora inicio",
      "Hora termino",
      task.done ? "Reabrir tarea" : "Completar tarea",
      "Eliminar tarea",
      "Volver"
    ];

    var selected = menu.show("Task #" + task.id, actions);
    if (selected < 0 || selected === 6) return;

    if (selected === 0) {
      dialog.viewText(renderTask(task), "Task #" + task.id);
    } else if (selected === 1) {
      editTitle(state, index);
    } else if (selected === 2) {
      editStartTime(state, index);
    } else if (selected === 3) {
      editEndTime(state, index);
    } else if (selected === 4) {
      if (task.done) reopenTask(state, index);
      else markDone(state, index);
    } else if (selected === 5) {
      deleteTask(state, index);
      return;
    }
  }
}

function sortTasks(tasks) {
  tasks.sort(function (a, b) {
    if (a.done !== b.done) return a.done ? 1 : -1;
    if (a.startTime && b.startTime) {
      if (a.startTime < b.startTime) return -1;
      if (a.startTime > b.startTime) return 1;
    } else if (a.startTime && !b.startTime) {
      return -1;
    } else if (!a.startTime && b.startTime) {
      return 1;
    }
    return a.id - b.id;
  });
}

function tasksMenu(state) {
  while (true) {
    sortTasks(state.tasks);

    var entries = [];
    var mapping = [];
    var i;
    for (i = 0; i < state.tasks.length; i++) {
      entries.push(formatTaskLine(state.tasks[i]));
      mapping.push(state.tasks[i].id);
    }
    entries.push("[+] Nueva tarea");
    entries.push("[i] Resumen");
    entries.push("[<] Volver");

    var selected = menu.show("Tareas", entries);
    if (selected < 0 || selected === entries.length - 1) return;
    if (selected === entries.length - 2) {
      dialog.viewText(renderSummary(state), "Resumen");
      continue;
    }
    if (selected === entries.length - 3) {
      createTask(state);
      continue;
    }
    taskDetailMenu(state, mapping[selected]);
  }
}

function historyMenu() {
  var history = readFileOrEmpty(HISTORY_FILE);
  if (!history) history = "Sin historial aun.\n";
  dialog.viewText(history, "Historial");
}

function exportTasksText(state) {
  sortTasks(state.tasks);
  var lines = [];
  lines.push("TASKS");
  lines.push("");
  if (state.tasks.length === 0) {
    lines.push("No hay tareas.");
  } else {
    var i;
    for (i = 0; i < state.tasks.length; i++) {
      var task = state.tasks[i];
      lines.push(formatTaskLine(task));
      lines.push("  " + taskSchedule(task));
    }
  }
  return lines.join("\n");
}

function main() {
  if (!ensureDir()) {
    dialog.error("No pude preparar el directorio en SD", true);
    return;
  }

  var state = loadState();
  if (!state.createdByApp) {
    state.createdByApp = "task_manager_ascii";
    saveState(state, "INIT APP");
  }

  while (true) {
    var choice = menu.show("Task ASCII", [
      "Tareas",
      "Nueva tarea",
      "Historial",
      "Ver listado ASCII",
      "Resumen",
      "Salir"
    ]);

    if (choice < 0 || choice === 5) return;
    if (choice === 0) tasksMenu(state);
    else if (choice === 1) createTask(state);
    else if (choice === 2) historyMenu();
    else if (choice === 3) dialog.viewText(exportTasksText(state), "Listado");
    else if (choice === 4) dialog.viewText(renderSummary(state), "Resumen");
  }
}

main();
