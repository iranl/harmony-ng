import { RemoteEngineBtInput } from './engine_bt_input.js';
import { t } from '../i18n.js';

export class RemoteEngineViews extends RemoteEngineBtInput {
  renderRemoteGrid() {
    const gridContainer = document.createElement('div');
    gridContainer.className = `remote-grid-5x8 ${this.editMode ? 'edit-mode' : ''}`;

    const grid = this.activeLayout.grid || [];

    for (let i = 0; i < 40; i++) {
      const btn = grid[i];
      if (btn && this.isButtonValid(btn)) {
        gridContainer.appendChild(this.createGridButtonEl(btn, i));
      } else {
        const slotEl = document.createElement('div');
        slotEl.className = `grid-slot-empty ${this.editMode ? 'clickable' : ''}`;
        if (this.editMode) {
          slotEl.innerHTML = `<span class="slot-add-icon">+</span>`;
          slotEl.title = t("tap_to_add_btn").replace("{slot}", i + 1);
          slotEl.addEventListener('click', () => {
            this.openSlotEditorModal(null, i);
          });
        }
        gridContainer.appendChild(slotEl);
      }
    }

    return gridContainer;
  }

  createGridButtonEl(btn, slotIndex) {
    const el = document.createElement('button');
    el.type = 'button';
    el.className = `remote-btn ${btn.variant ? 'btn-' + btn.variant : ''} ${this.editMode ? 'in-edit' : ''}`;
    el.setAttribute('data-slot', slotIndex);

    el.innerHTML = `
      <span class="btn-label">${btn.label || btn.command || 'Btn'}</span>
      ${this.editMode ? `<span class="btn-edit-badge">✎</span>` : ''}
    `;

    el.addEventListener('click', (e) => {
      e.stopPropagation();
      this.triggerHaptic();
      if (this.editMode) {
        this.openSlotEditorModal(btn, slotIndex);
      } else {
        if (btn.deviceId && btn.command && this.onSendCallback) {
          el.classList.add('active-press');
          setTimeout(() => el.classList.remove('active-press'), 150);
          this.onSendCallback(btn.deviceId, btn.command);
        }
      }
    });

    return el;
  }

  renderAllCommandsView() {
    const view = document.createElement('div');
    view.className = 'all-commands-panel';

    const searchInput = document.createElement('input');
    searchInput.type = 'text';
    searchInput.className = 'form-control cmd-search-input';
    searchInput.placeholder = t('placeholder_search_commands');
    searchInput.value = this.cmdSearchFilter;
    view.appendChild(searchInput);

    const listContainer = document.createElement('div');
    listContainer.className = 'commands-category-list';
    view.appendChild(listContainer);

    const targetDevices = this.currentScope === 'device'
      ? (this.currentTarget ? [this.currentTarget] : [])
      : this.availableDevices;

    const renderList = (filterText) => {
      listContainer.innerHTML = '';
      const q = filterText.trim().toLowerCase();

      targetDevices.forEach(dev => {
        const rawCmds = this.deviceCommandsMap[dev.id] || [];
        const filteredCmds = rawCmds.filter(c => {
          const name = String(c.name || c).toLowerCase();
          return !q || name.includes(q);
        });

        if (filteredCmds.length === 0 && q) return;

        const devSec = document.createElement('div');
        devSec.className = 'cmd-dev-section';

        if (targetDevices.length > 1) {
          const title = document.createElement('div');
          title.className = 'cmd-dev-header';
          title.textContent = `${dev.name || dev.label} (${filteredCmds.length})`;
          devSec.appendChild(title);
        }

        const chipsGrid = document.createElement('div');
        chipsGrid.className = 'cmds-grid';

        filteredCmds.forEach(c => {
          const cmdName = c.name || c;
          const chip = document.createElement('button');
          chip.type = 'button';
          chip.className = 'btn btn-xs btn-ghost cmd-chip';
          chip.textContent = cmdName;
          chip.addEventListener('click', async () => {
            await this.triggerHaptic();
            chip.classList.add('active-press');
            setTimeout(() => chip.classList.remove('active-press'), 150);
            if (this.onSendCallback) {
              this.onSendCallback(dev.id, cmdName);
            }
          });
          chipsGrid.appendChild(chip);
        });

        devSec.appendChild(chipsGrid);
        listContainer.appendChild(devSec);
      });

      if (listContainer.children.length === 0) {
        listContainer.innerHTML = `<div class="muted text-center p-2 mini">${t('no_matching_commands')}</div>`;
      }
    };

    renderList(this.cmdSearchFilter);

    searchInput.addEventListener('input', (e) => {
      this.cmdSearchFilter = e.target.value;
      renderList(this.cmdSearchFilter);
    });

    return view;
  }

  renderKeypadView() {
    const view = document.createElement('div');
    view.className = 'keypad-panel';

    const numDevs = this.getDevicesWithNumbers();
    if (numDevs.length === 0) {
      view.innerHTML = `<div class="muted text-center p-2 mini">${t('no_keypad_commands')}</div>`;
      return view;
    }

    // Default to first device in activity based on order
    if (!this.selectedKeypadDeviceId || !numDevs.some(d => String(d.id) === String(this.selectedKeypadDeviceId))) {
      this.selectedKeypadDeviceId = numDevs[0].id;
    }

    const currentDev = numDevs.find(d => String(d.id) === String(this.selectedKeypadDeviceId)) || numDevs[0];
    const cmds = this.deviceCommandsMap[currentDev.id] || [];

    const findNumCmd = (num) => {
      const match = cmds.find(c => {
        const n = String(c.name || c).toLowerCase().replace(/[\s_\-]/g, '');
        return n === String(num) || n === `number${num}` || n === `num${num}` || n === `digit${num}`;
      });
      return match ? (match.name || match) : String(num);
    };

    if (numDevs.length > 1) {
      const selectBar = document.createElement('div');
      selectBar.className = 'keypad-select-bar';
      selectBar.innerHTML = `
        <label class="keypad-select-label">${t('target_device')}</label>
        <select class="form-control keypad-dev-select">
          ${numDevs.map(d => `<option value="${d.id}" ${String(d.id) === String(currentDev.id) ? 'selected' : ''}>${d.name || d.label} (${d.model || d.id})</option>`).join('')}
        </select>
      `;

      const selectEl = selectBar.querySelector('select');
      selectEl.addEventListener('change', () => {
        this.selectedKeypadDeviceId = selectEl.value;
        this.updateDOM();
      });

      view.appendChild(selectBar);
    }

    const keys = [
      '1', '2', '3',
      '4', '5', '6',
      '7', '8', '9',
      '', '0', ''
    ];

    const grid = document.createElement('div');
    grid.className = 'keypad-grid';

    keys.forEach(k => {
      if (k === '') {
        const spacer = document.createElement('div');
        spacer.className = 'btn-spacer';
        grid.appendChild(spacer);
      } else {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'remote-btn num-key-btn';
        btn.textContent = k;
        btn.addEventListener('click', async () => {
          await this.triggerHaptic();
          btn.classList.add('active-press');
          setTimeout(() => btn.classList.remove('active-press'), 150);
          if (currentDev && this.onSendCallback) {
            const actualCmd = findNumCmd(k);
            this.onSendCallback(currentDev.id, actualCmd);
          }
        });
        grid.appendChild(btn);
      }
    });

    view.appendChild(grid);
    return view;
  }


  openSlotEditorModal(btn, slotIndex) {
    let modal = document.getElementById('buttonEditorModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'buttonEditorModal';
      modal.className = 'modal-backdrop';
      document.body.appendChild(modal);
    }

    const isEdit = !!btn && this.isButtonValid(btn);
    const allowedDevices = this.currentScope === 'device'
      ? (this.currentTarget ? [this.currentTarget] : [])
      : this.availableDevices;

    let initialDevId = isEdit ? btn.deviceId : (allowedDevices[0] ? allowedDevices[0].id : '');
    if (!allowedDevices.some(d => String(d.id) === String(initialDevId))) {
      initialDevId = allowedDevices[0] ? allowedDevices[0].id : '';
    }

    const devOptions = allowedDevices.map(d =>
      `<option value="${d.id}" ${String(d.id) === String(initialDevId) ? 'selected' : ''}>${d.name || d.label} (${d.model || d.id})</option>`
    ).join('');

    const cmds = this.deviceCommandsMap[initialDevId] || [];
    const currentCmd = isEdit ? btn.command : '';
    const cmdOptions = cmds.map(c => {
      const name = c.name || c;
      return `<option value="${name}" ${name === currentCmd ? 'selected' : ''}>${name}</option>`;
    }).join('');

    const row = Math.floor(slotIndex / 5) + 1;
    const col = (slotIndex % 5) + 1;

    modal.innerHTML = `
      <div class="modal-card">
        <div class="modal-header">
          <h3>${isEdit ? t('customize_button') : t('add_button')} (${t('row_col_format').replace('{row}', row).replace('{col}', col)})</h3>
          <button type="button" class="btn btn-ghost btn-sm" id="btnModalClose">✕</button>
        </div>
        <div class="modal-body">
          <div class="form-group">
            <label>${t('target_device')} ${this.currentScope === 'device' ? `<span class="mini muted">${t('current_device_label')}</span>` : ''}</label>
            <select id="editBtnDevice" class="form-control" ${this.currentScope === 'device' ? 'disabled' : ''}>
              ${devOptions || `<option value="">${t('no_devices_avail')}</option>`}
            </select>
          </div>
          <div class="form-group">
            <label>${t('cmd_function')}</label>
            <select id="editBtnCommand" class="form-control">
              ${cmdOptions || `<option value="">${t('no_commands_avail')}</option>`}
            </select>
          </div>
          <div class="form-group">
            <label>${t('button_label')}</label>
            <input type="text" id="editBtnLabel" class="form-control" value="${isEdit ? (btn.label || '') : ''}" placeholder="${t('placeholder_btn_label')}">
          </div>
          <div class="form-group">
            <label>${t('color_accent')}</label>
            <select id="editBtnVariant" class="form-control">
              <option value="" ${!isEdit || !btn.variant ? 'selected' : ''}>${t('variant_dark')}</option>
              <option value="primary" ${isEdit && btn.variant === 'primary' ? 'selected' : ''}>${t('variant_primary')}</option>
              <option value="accent" ${isEdit && btn.variant === 'accent' ? 'selected' : ''}>${t('variant_accent')}</option>
              <option value="warning" ${isEdit && btn.variant === 'warning' ? 'selected' : ''}>${t('variant_warning')}</option>
              <option value="danger" ${isEdit && btn.variant === 'danger' ? 'selected' : ''}>${t('variant_danger')}</option>
            </select>
          </div>
        </div>
        <div class="modal-footer">
          ${btn ? `<button type="button" class="btn btn-danger btn-sm" id="btnModalDelete">${t('clear_slot')}</button>` : ''}
          <div style="flex:1;"></div>
          <button type="button" class="btn btn-secondary btn-sm" id="btnModalCancel">${t('cancel')}</button>
          <button type="button" class="btn btn-primary btn-sm" id="btnModalSave">${t('save')}</button>
        </div>
      </div>
    `;
    modal.style.display = 'flex';

    const devSelect = modal.querySelector('#editBtnDevice');
    const cmdSelect = modal.querySelector('#editBtnCommand');
    const labelInput = modal.querySelector('#editBtnLabel');
    const saveBtn = modal.querySelector('#btnModalSave');

    const updateSaveState = () => {
      saveBtn.disabled = !cmdSelect.value;
    };
    updateSaveState();

    devSelect.addEventListener('change', () => {
      const newDevId = devSelect.value;
      const newCmds = this.deviceCommandsMap[newDevId] || [];
      cmdSelect.innerHTML = newCmds.map(c => {
        const name = c.name || c;
        return `<option value="${name}">${name}</option>`;
      }).join('') || '<option value="">(No commands available)</option>';
      if (cmdSelect.value) {
        labelInput.value = cmdSelect.value;
      } else {
        labelInput.value = '';
      }
      updateSaveState();
    });

    cmdSelect.addEventListener('change', () => {
      if (!labelInput.value || labelInput.value === currentCmd) {
        labelInput.value = cmdSelect.value;
      }
      updateSaveState();
    });

    const closeModal = () => { modal.style.display = 'none'; };
    modal.querySelector('#btnModalClose').addEventListener('click', closeModal);
    modal.querySelector('#btnModalCancel').addEventListener('click', closeModal);

    const deleteBtn = modal.querySelector('#btnModalDelete');
    if (deleteBtn) {
      deleteBtn.addEventListener('click', async () => {
        this.activeLayout.grid[slotIndex] = null;
        await saveRemoteLayout(this.currentScope, this.currentTarget.id, this.activeLayout);
        closeModal();
        this.updateDOM();
      });
    }

    saveBtn.addEventListener('click', async () => {
      const chosenDevId = this.currentScope === 'device' ? this.currentTarget.id : devSelect.value;
      const chosenCmd = cmdSelect.value;
      if (!chosenDevId || !chosenCmd) {
        alert(t('select_device_cmd'));
        return;
      }

      this.activeLayout.grid[slotIndex] = {
        id: `btn_${slotIndex}`,
        deviceId: chosenDevId,
        command: chosenCmd,
        label: labelInput.value.trim() || chosenCmd,
        variant: modal.querySelector('#editBtnVariant').value
      };

      await saveRemoteLayout(this.currentScope, this.currentTarget.id, this.activeLayout);
      closeModal();
      this.updateDOM();
    });
  }
}
