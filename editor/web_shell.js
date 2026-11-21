(() => {
  const canvas = document.getElementById('canvas');
  const newProject = document.getElementById('new-project');
  const newDialog = document.getElementById('new-project-dialog');
  const newForm = document.getElementById('new-project-form');
  const newName = document.getElementById('new-project-name');
  const newError = document.getElementById('new-project-error');
  const choose = document.getElementById('import-project');
  const input = document.getElementById('project-zip');
  const download = document.getElementById('export-project');
  const status = document.getElementById('editor-status');
  const importPath = '/editor-import.zip';
  const exportPath = '/editor-export.zip';
  const maxInput = 128 * 1024 * 1024; // Must match the C++ archive limit.
  let ready = false;
  let busy = false;
  let projectName = '';

  function refreshButtons() {
    newProject.disabled = !ready || busy;
    choose.disabled = !ready || busy;
    download.disabled = !ready || busy || !projectName;
  }

  function confirmReplacement() {
    return !projectName || window.confirm('Replace the current project? Edits not in a downloaded ZIP will be lost.');
  }

  Module['canvas'] = canvas;
  Module['editorReady'] = () => {
    ready = true;
    resizeCanvas();
    status.textContent = 'Create a new project or import a project ZIP to begin.';
    refreshButtons();
  };
  Module['printErr'] = (message) => {
    console.error(message);
    if (!ready) status.textContent = message;
  };
  Module['onAbort'] = (message) => {
    ready = false;
    status.textContent = `Editor stopped: ${message}`;
    refreshButtons();
  };

  canvas.addEventListener('contextmenu', event => event.preventDefault());
  canvas.addEventListener('pointerdown', () => canvas.focus({preventScroll: true}));
  function resizeCanvas() {
    if (!ready) return;
    Module['_web_resize_canvas'](Math.max(1, canvas.clientWidth), Math.max(1, canvas.clientHeight));
  }
  new ResizeObserver(resizeCanvas).observe(canvas);

  canvas.addEventListener('keydown', event => {
    if (event.ctrlKey && event.key.toLowerCase() === 's') event.preventDefault();
  });

  newProject.addEventListener('click', () => {
    newForm.reset();
    newError.textContent = '';
    newDialog.showModal();
  });
  document.getElementById('cancel-new-project').addEventListener('click', () => newDialog.close());
  newForm.addEventListener('submit', event => {
    event.preventDefault();
    if (!ready || busy || !confirmReplacement()) return;
    const name = newName.value.trim();
    busy = true;
    refreshButtons();
    try {
      const error = Module['ccall']('web_create_project', 'string', ['string'], [name]);
      if (error) throw new Error(error);
      projectName = name;
      newDialog.close();
      status.textContent = `${name} created. Save edits in the editor, then export a ZIP to keep them.`;
      canvas.focus({preventScroll: true});
    } catch (error) {
      newError.textContent = `Could not create project: ${error.message}`;
      newName.focus();
    } finally {
      busy = false;
      refreshButtons();
    }
  });

  choose.addEventListener('click', () => input.click());
  input.addEventListener('change', async () => {
    const file = input.files[0];
    input.value = '';
    if (!file || busy || !ready) return;
    if (file.size > maxInput) {
      status.textContent = 'Project ZIP is larger than 128 MiB.';
      return;
    }
    if (!confirmReplacement()) return;

    busy = true;
    refreshButtons();
    status.textContent = 'Importing project…';
    const fs = Module['FS'];
    let uploaded = false;
    try {
      fs.writeFile(importPath, new Uint8Array(await file.arrayBuffer()), {canOwn: true});
      uploaded = true;
      const error = Module['ccall']('web_import_project', 'string', [], []);
      if (error) throw new Error(error);
      projectName = file.name.replace(/\.zip$/i, '') || file.name;
      status.textContent = `${file.name} opened. Save edits in the editor, then export a ZIP to keep them.`;
    } catch (error) {
      status.textContent = `Import failed: ${error.message}`;
    } finally {
      if (uploaded) fs.unlink(importPath);
      busy = false;
      refreshButtons();
      canvas.focus({preventScroll: true});
    }
  });

  download.addEventListener('click', async () => {
    if (!ready || busy || !projectName) return;
    busy = true;
    refreshButtons();
    canvas.inert = true;
    status.textContent = 'Exporting project ZIP…';
    try {
      // Leave one rendering opportunity before synchronous ZIP packaging.
      await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      if (!ready) return;
      const fs = Module['FS'];
      const error = Module['ccall']('web_export_project', 'string', [], []);
      if (error) throw new Error(error);
      let bytes;
      try { bytes = fs.readFile(exportPath); }
      finally { fs.unlink(exportPath); }
      const url = URL.createObjectURL(new Blob([bytes], {type: 'application/zip'}));
      const link = document.createElement('a');
      link.href = url;
      link.download = `${projectName}-edited.zip`;
      document.body.append(link);
      link.click();
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 30_000);
      status.textContent = `Exported ${link.download}.`;
    } catch (error) {
      status.textContent = `Export failed: ${error.message}`;
    } finally {
      busy = false;
      refreshButtons();
      canvas.inert = false;
      canvas.focus({preventScroll: true});
    }
  });
})();
