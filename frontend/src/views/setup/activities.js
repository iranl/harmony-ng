// Activity Setup and Editor View
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { openIconPicker } from '../../components/icon-picker.js';
import { renderActivityIcon } from '../../icons/render.js';
import { createSequenceEditor } from '../../components/sequence-editor.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-activities', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let selectedActivity = null;

  function renderList() {
    container.innerHTML = '';

    const activities = store.get('activities.list') || [];
    const header = h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('activities_setup', 'Activities') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: `+ ${t('add_activity', 'Add Activity')}`,
        onclick: () => {
          selectedActivity = {
            id: '',
            name: 'New Activity',
            type: 'Custom',
            icon: '',
            deviceIds: [],
            startSequence: [],
            stopSequence: [],
          };
          renderEditor();
        },
      }),
    ]);

    const actList = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });

    if (activities.length === 0) {
      actList.appendChild(h('div', { className: 'text-dim', text: 'No activities configured.' }));
    } else {
      activities.forEach(act => {
        const iconEl = renderActivityIcon(act, 'activity-icon');
        const row = h('div', {
          className: 'card',
          style: {
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            padding: '12px',
            margin: 0,
            cursor: 'pointer',
          },
          onclick: () => {
            selectedActivity = JSON.parse(JSON.stringify(act));
            renderEditor();
          },
        }, [
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '12px' } }, [
            iconEl,
            h('div', {}, [
              h('div', { style: { fontWeight: 'bold', fontSize: '14px' }, text: act.name }),
              h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: `${(act.deviceIds || []).length} assigned devices` }),
            ]),
          ]),
          h('button', {
            className: 'btn btn-secondary btn-sm',
            text: t('edit', 'Edit'),
          }),
        ]);

        actList.appendChild(row);
      });
    }

    container.appendChild(header);
    container.appendChild(actList);
  }

  function renderEditor() {
    container.innerHTML = '';
    const act = selectedActivity;
    const devices = store.get('devices') || [];
    const commandsMap = store.get('commandsMap') || {};

    const backBtn = h('button', {
      className: 'btn btn-secondary btn-sm',
      text: `‹ ${t('back_to_activities', 'Back')}`,
      onclick: () => {
        selectedActivity = null;
        renderList();
      },
    });

    const header = h('div', { className: 'card-header' }, [
      backBtn,
      h('h3', { className: 'card-title', text: act.id ? act.name : 'Create Activity' }),
    ]);

    // Name & Icon form
    const nameInput = h('input', {
      type: 'text',
      className: 'form-input',
      value: act.name,
      oninput: (e) => { act.name = e.target.value; },
    });

    const iconPreview = h('div', {
      style: { display: 'flex', alignItems: 'center', gap: '10px', marginTop: '6px' },
    }, [
      renderActivityIcon(act, 'activity-icon'),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('change_icon', 'Change Icon...'),
        onclick: () => {
          openIconPicker(act.icon, (newIcon) => {
            act.icon = newIcon;
            renderEditor();
          });
        },
      }),
    ]);

    const infoCard = h('div', { className: 'card' }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Activity Name' }),
        nameInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Icon' }),
        iconPreview,
      ]),
    ]);

    // Assigned Devices
    const devCheckboxes = devices.map(dev => {
      const isChecked = (act.deviceIds || []).includes(String(dev.id));
      const cb = h('input', {
        type: 'checkbox',
        checked: isChecked,
        onchange: (e) => {
          if (!act.deviceIds) act.deviceIds = [];
          if (e.target.checked) {
            if (!act.deviceIds.includes(String(dev.id))) act.deviceIds.push(String(dev.id));
          } else {
            act.deviceIds = act.deviceIds.filter(id => id !== String(dev.id));
          }
        },
      });

      return h('label', {
        style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', padding: '4px 0' },
      }, [
        cb,
        h('span', { style: { fontSize: '13px' }, text: dev.name }),
        h('span', { className: 'badge', text: dev.type || 'IR' }),
      ]);
    });

    const devicesCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-title', text: t('assigned_devices', 'Assigned Devices') }),
      h('div', { style: { display: 'flex', flexDirection: 'column', gap: '4px', marginTop: '8px' } }, devCheckboxes),
    ]);

    // Start Sequence
    const startSeqCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('div', { className: 'card-title', text: t('startup_sequence', 'Startup Sequence') }),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: '▶ Test Sequence',
          onclick: async () => {
            try {
              toast.info('Running sequence test...');
              await api.testActivitySequence(JSON.stringify(act.startSequence));
              toast.success('Sequence completed');
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]),
      createSequenceEditor(act.startSequence || [], devices, commandsMap, (newSteps) => {
        act.startSequence = newSteps;
      }),
    ]);

    // Stop Sequence
    const stopSeqCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('div', { className: 'card-title', text: t('shutdown_sequence', 'Shutdown Sequence') }),
      ]),
      createSequenceEditor(act.stopSequence || [], devices, commandsMap, (newSteps) => {
        act.stopSequence = newSteps;
      }),
    ]);

    // Bottom Action Buttons
    const actionsCard = h('div', { style: { display: 'flex', justifyContent: 'space-between', gap: '10px' } }, [
      act.id ? h('button', {
        className: 'btn btn-danger',
        text: t('delete_activity', 'Delete Activity'),
        onclick: async () => {
          const ok = await confirm.ask({
            title: t('delete_activity', 'Delete Activity'),
            message: `Are you sure you want to delete ${act.name}?`,
            danger: true,
          });
          if (ok) {
            try {
              await api.deleteActivity(act.id);
              toast.success('Activity deleted');
              const acts = await api.getActivities();
              store.set('activities.list', acts.activities || []);
              selectedActivity = null;
              renderList();
            } catch (e) {
              toast.error(e.message);
            }
          }
        },
      }) : h('div'),

      h('button', {
        className: 'btn btn-primary',
        text: t('save_activity', 'Save Activity'),
        onclick: async () => {
          try {
            await api.saveActivity(act);
            toast.success(t('activity_saved', 'Activity saved!'));
            const acts = await api.getActivities();
            store.set('activities.list', acts.activities || []);
            selectedActivity = null;
            renderList();
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]);

    container.appendChild(header);
    container.appendChild(infoCard);
    container.appendChild(devicesCard);
    container.appendChild(startSeqCard);
    container.appendChild(stopSeqCard);
    container.appendChild(actionsCard);
  }

  renderList();
  return container;
}

export default createView;
