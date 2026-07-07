
function toggleTheme() {
  var isDark = document.documentElement.getAttribute('data-theme') === 'dark';
  if (isDark) {
    document.documentElement.removeAttribute('data-theme');
    try { localStorage.setItem('theme', 'light'); } catch (e) {}
  } else {
    document.documentElement.setAttribute('data-theme', 'dark');
    try { localStorage.setItem('theme', 'dark'); } catch (e) {}
  }
}
(function(){
  var el = document.getElementById('livePowerBadge');
  if (!el) return;
  function poll(){
    fetch('/livepower').then(function(r){ return r.json(); }).then(function(data){
      el.innerHTML = data.text || '';
    }).catch(function(e){ /* naechster Versuch beim naechsten Intervall */ });
  }
  poll();
  setInterval(poll, 2500);
})();
function showToast(msg, type) {
  type = type || 'info';
  var host = document.getElementById('toastHost');
  if (!host) return;
  var t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = msg;
  host.appendChild(t);
  requestAnimationFrame(function(){ t.classList.add('show'); });
  setTimeout(function(){
    t.classList.remove('show');
    setTimeout(function(){ t.remove(); }, 250);
  }, 3200);
}
(function(){
  var p = new URLSearchParams(location.search);
  if (p.has('saved')) {
    var ok = p.get('saved') !== '0';
    showToast(ok ? 'Gespeichert' : 'Fehler beim Speichern', ok ? 'ok' : 'err');
    history.replaceState(null, '', location.pathname);
  }
})();
document.querySelectorAll('form').forEach(function(f){
  f.addEventListener('submit', function(){
    var btn = f.querySelector('button[type="submit"]');
    if (btn && !btn.disabled) {
      btn.dataset.origText = btn.innerText;
      btn.innerText = 'Speichere...';
      // WICHTIG: btn.disabled erst NACH diesem Tick setzen (setTimeout 0).
      // Bei submit-buttons mit name/value (z.B. name='formType' value='xyz')
      // wird ein waehrend des submit-Events deaktivierter Button aus den
      // gesendeten Formulardaten ausgeschlossen - der Server erhaelt dann
      // ein leeres formType und keine der Aktionen greift, ohne Fehlermeldung.
      setTimeout(function(){ btn.disabled = true; }, 0);
    }
  });
});
function fwBadge(cls, text, title) {
  var b = document.getElementById('fwVersionBadge');
  if (!b) return;
  b.className = 'badge' + (cls ? ' ' + cls : '');
  b.innerText = text;
  if (title) b.title = title;
}
function applyFwResult(j) {
  if (!j) return;
  if (!j.ok) {
    fwBadge('errb', 'v' + j.currentVersion + ' ⚠', 'Update-Check fehlgeschlagen: ' + (j.error || 'unbekannter Fehler') + ' - klicke zum erneuten Versuch');
  } else if (j.updateAvailable) {
    fwBadge('warnb', 'v' + j.currentVersion + ' → ' + j.latestVersion, 'Update verfügbar (' + j.latestVersion + ') - siehe Konto-Seite');
  } else {
    fwBadge('okb', 'v' + j.currentVersion, 'Firmware ist aktuell');
  }
}
(function(){
  // Seiten laden alle 60s per meta-refresh neu (siehe htmlHeader), daher den
  // zuletzt bekannten Check-Stand aus localStorage sofort anzeigen, statt bei
  // jedem Reload kurz auf den neutralen Standardzustand zurueckzufallen.
  try {
    var cached = localStorage.getItem('fwCheckResult');
    if (cached) applyFwResult(JSON.parse(cached));
  } catch (e) {}
})();
async function checkFirmwareVersion(force) {
  var last = 0;
  try { last = parseInt(localStorage.getItem('fwCheckAt') || '0', 10); } catch (e) {}
  if (!force && Date.now() - last < 10 * 60 * 1000) return;
  if (force) fwBadge('warnb', 'Prüfe...', 'Frage GitHub nach der neuesten Version...');
  try {
    const r = await fetch('/versioncheck', { cache: 'no-store' });
    const j = await r.json();
    applyFwResult(j);
    try {
      localStorage.setItem('fwCheckAt', String(Date.now()));
      localStorage.setItem('fwCheckResult', JSON.stringify(j));
    } catch (e) {}
  } catch (e) {
    fwBadge('errb', 'v?  ⚠', 'Verbindung zum Geraet fehlgeschlagen - klicke zum erneuten Versuch');
  }
}
checkFirmwareVersion(false);
setInterval(function(){ checkFirmwareVersion(false); }, 10 * 60 * 1000);
(function(){
  var b = document.getElementById('fwVersionBadge');
  if (b) {
    b.style.cursor = 'pointer';
    b.addEventListener('click', function(){ checkFirmwareVersion(true); });
  }
})();
