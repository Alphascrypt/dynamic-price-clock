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
      btn.disabled = true;
      btn.dataset.origText = btn.innerText;
      btn.innerText = 'Speichere...';
    }
  });
});
