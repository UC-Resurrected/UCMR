const boxes = [...document.querySelectorAll('[data-completes]')];
const labels = {
  0: 'Start the import steps below',
  1: 'Your game source is ready',
  2: 'Game files located',
  3: 'Executable ready for import',
  4: 'Import complete — connect your account',
  5: 'Ready for multiplayer'
};

function updateProgress() {
  const complete = boxes.filter((box) => box.checked).length;
  document.getElementById('readiness-copy').textContent = labels[complete];
  document.querySelectorAll('.milestones li').forEach((item, index) => {
    item.classList.toggle('done', index < complete);
  });
  localStorage.setItem('uncharted-multiplayer-guide', JSON.stringify(boxes.map((box) => box.checked)));
}

try {
  const saved = JSON.parse(localStorage.getItem('uncharted-multiplayer-guide'));
  if (Array.isArray(saved)) boxes.forEach((box, index) => { box.checked = Boolean(saved[index]); });
} catch { /* First visit or unavailable storage. */ }

boxes.forEach((box) => box.addEventListener('change', updateProgress));
updateProgress();
