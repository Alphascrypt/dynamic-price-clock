
function ghBadge(cls, text) {
  var b = document.getElementById('ghUpdateBadge');
  if (b) { b.className = 'badge ' + cls; b.innerText = text; }
}
async function checkGhUpdate() {
  var msg = document.getElementById('ghUpdateMsg');
  var btn = document.getElementById('ghUpdateBtn');
  ghBadge('warnb', 'Pruefe...');
  msg.innerText = '';
  btn.style.display = 'none';
  try {
    const r = await fetch('/checkgithubupdate', { cache: 'no-store' });
    const j = await r.json();
    if (!j.ok) {
      ghBadge('errb', 'Fehler');
      msg.innerText = j.error || 'Unbekannter Fehler beim Pruefen.';
      return;
    }
    if (j.updateAvailable) {
      ghBadge('warnb', 'Update verfuegbar');
      msg.innerText = 'Installiert: ' + j.currentVersion + '  ->  Neu: ' + j.latestVersion;
      window.ghUpdateUrl = j.downloadUrl;
      btn.style.display = '';
    } else {
      ghBadge('okb', 'Aktuell');
      msg.innerText = 'Du hast bereits die neueste Version (' + j.currentVersion + ').';
    }
  } catch (e) {
    ghBadge('errb', 'Fehler');
    msg.innerText = 'Verbindung fehlgeschlagen: ' + e;
  }
}
function formatBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(0) + ' KB';
  return (n / (1024 * 1024)).toFixed(1) + ' MB';
}
var _ghPollStart = 0;
var _ghLastBytes = 0;
var _ghLastBytesTime = 0;
var _ghTimeoutMs = 5 * 60 * 1000;
function ghSetPhase(icon, lbl, sub) {
  var i=document.getElementById('ghPhaseIcon');
  var l=document.getElementById('ghPhaseLbl');
  var s=document.getElementById('ghProgressSub');
  if(i)i.innerText=icon;
  if(l)l.innerText=lbl;
  if(s&&sub!=null)s.innerText=sub;
}
async function pollGhProgress() {
  var wrap = document.getElementById('ghProgressWrap');
  var bar = document.getElementById('ghProgressBar');
  var text = document.getElementById('ghProgressText');
  var speed = document.getElementById('ghSpeedText');
  var msg = document.getElementById('ghUpdateMsg');
  var now = Date.now();

  if (now - _ghPollStart > _ghTimeoutMs) {
    ghBadge('errb', 'Timeout');
    ghSetPhase('&#9888;', 'Timeout', 'Das Update hat zu lange gebraucht. Bitte Verbindung pruefen und erneut versuchen.');
    bar.style.background = 'var(--danger)';
    return;
  }

  try {
    const r = await fetch('/otaprogress', { cache: 'no-store' });
    const j = await r.json();

    var hbAge = (typeof j.heartbeatAge === 'number') ? j.heartbeatAge : 0;
    if (j.percent < 0 || j.bytesTotal === 0) {
      var waitSec = Math.round((now - _ghPollStart) / 1000);
      var hint = hbAge > 45 ? 'Keine Aktivitaet seit ' + hbAge + 's — ESP32 hängt möglicherweise. Bitte Gerät neu starten.' : 'GitHub leitet den Download um — das dauert 5-15 Sekunden.' + (j.diag ? ' (' + j.diag + ')' : '');
      ghSetPhase('&#8987;', 'Verbinde & lade herunter... (' + waitSec + 's)', hint);
      text.innerText = '';
      if(speed) speed.innerText = '';
    } else if (j.percent < 100) {
      ghSetPhase('&#8659;', 'Herunterladen...', '');
      bar.style.width = j.percent + '%';
      text.innerText = j.percent + '% (' + formatBytes(j.bytesWritten) + ' / ' + formatBytes(j.bytesTotal) + ')';
      if (speed && j.bytesWritten > _ghLastBytes && _ghLastBytesTime > 0) {
        var dt = (now - _ghLastBytesTime) / 1000;
        var bps = (j.bytesWritten - _ghLastBytes) / dt;
        speed.innerText = formatBytes(bps) + '/s';
      }
      _ghLastBytes = j.bytesWritten;
      _ghLastBytesTime = now;
    } else {
      ghSetPhase('&#9889;', 'Flashe Firmware...', 'Bitte Stromversorgung nicht trennen.');
      bar.style.width = '100%';
      text.innerText = 'Schreibe Flash...';
    }

    if (j.done) {
      if (j.success) {
        bar.style.width = '100%';
        ghBadge('okb', 'Fertig');
        ghSetPhase('&#10003;', 'Update erfolgreich!', 'Das Geraet startet neu — Seite wird in 8 Sekunden neu geladen.');
        msg.innerText = '';
        setTimeout(function(){ location.reload(); }, 8000);
      } else {
        ghBadge('errb', 'Fehler');
        ghSetPhase('&#10008;', 'Update fehlgeschlagen', j.error || 'Unbekannter Fehler.');
        wrap.style.display = 'none';
      }
      return;
    }

    setTimeout(pollGhProgress, 300);
  } catch (e) {
    bar.style.width = '100%';
    ghBadge('okb', 'Neustart');
    ghSetPhase('&#10003;', 'Neustart laeuft...', 'Verbindung unterbrochen — das Geraet startet neu. Seite wird in 8 Sekunden geladen.');
    setTimeout(function(){ location.reload(); }, 8000);
  }
}
function upBadge(cls, text) {
  var b = document.getElementById('upBadge');
  if (b) { b.className = 'badge ' + cls; b.innerText = text; }
}
function startUpload(ev) {
  ev.preventDefault();
  var file = document.getElementById('upFile').files[0];
  if (!file) return false;
  if (!file.name.toLowerCase().endsWith('.bin')) {
    alert('Bitte eine .bin-Datei waehlen.');
    return false;
  }
  if (!confirm('Firmware "' + file.name + '" jetzt installieren? Das Geraet startet danach automatisch neu. Falls die Datei nicht zur Hardware passt, kann ein serieller Flash noetig sein.')) return false;

  var wrap = document.getElementById('upProgressWrap');
  var bar = document.getElementById('upProgressBar');
  var text = document.getElementById('upProgressText');
  var msg = document.getElementById('upMsg');
  var sub = document.getElementById('upSubmit');

  wrap.style.display = '';
  bar.style.width = '0%';
  msg.innerText = '';
  sub.disabled = true;
  upBadge('warnb', 'Lade hoch...');

  var fd = new FormData();
  fd.append('firmware', file);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/uploadfirmware', true);
  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      var p = Math.round((e.loaded / e.total) * 100);
      bar.style.width = p + '%';
      text.innerText = p + '% (' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total) + ')';
    }
  };
  xhr.onload = function() {
    try {
      var j = JSON.parse(xhr.responseText);
      if (j.ok) {
        bar.style.width = '100%';
        upBadge('okb', 'Fertig');
        msg.innerText = 'Upload erfolgreich, das Geraet startet jetzt neu...';
      } else {
        upBadge('errb', 'Fehler');
        msg.innerText = j.error || 'Upload fehlgeschlagen.';
        sub.disabled = false;
      }
    } catch(e) {
      upBadge('errb', 'Fehler');
      msg.innerText = 'Antwort konnte nicht gelesen werden.';
      sub.disabled = false;
    }
  };
  xhr.onerror = function() {
    upBadge('warnb', 'Neustart');
    msg.innerText = 'Verbindung unterbrochen - das ist beim Neustart normal.';
  };
  xhr.send(fd);
  return false;
}
async function startGhUpdate() {
  if (!window.ghUpdateUrl) return;
  if (!confirm('Firmware jetzt aktualisieren? Das Geraet startet danach automatisch neu und ist kurz nicht erreichbar.')) return;
  var msg = document.getElementById('ghUpdateMsg');
  var wrap = document.getElementById('ghProgressWrap');
  var bar = document.getElementById('ghProgressBar');
  ghBadge('warnb', 'Aktualisiere...');
  msg.innerText = '';
  wrap.style.display = '';
  bar.style.width = '0%';
  _ghPollStart = Date.now();
  _ghLastBytes = 0;
  _ghLastBytesTime = 0;
  ghSetPhase('&#8987;', 'Verbinde mit GitHub...', 'GitHub leitet den Download um — das dauert 5-15 Sekunden.');
  try {
    const body = new URLSearchParams();
    body.set('url', window.ghUpdateUrl);
    const r = await fetch('/githubupdate', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' }, body: body.toString() });
    const j = await r.json();
    if (j.ok) {
      setTimeout(pollGhProgress, 300);
    } else {
      ghBadge('errb', 'Fehler');
      msg.innerText = j.error || 'Update fehlgeschlagen.';
      wrap.style.display = 'none';
    }
  } catch (e) {
    msg.innerText = 'Verbindung unterbrochen - das ist beim Neustart normal.';
  }
}
