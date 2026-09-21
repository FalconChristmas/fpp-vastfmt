<?php
    // The cape defaults ship ResetPin as a bare GPIO number (the K16A-B uses
    // "2"), while the settings menu lists pin names. Translate a numeric value
    // to its pin name once, so the menu can match it - otherwise the select
    // shows no match, and saving the page would quietly point reset at
    // whichever pin happened to be first in the list.
    $curGpio = ReadSettingFromFile("ResetPin", "fpp-vastfmt");
    if ($curGpio !== "" && $curGpio !== false && ctype_digit((string)$curGpio)) {
        $defaultGPIOChip = $settings['BeaglePlatform'] ? 3 : 0;
        // Go through Apache's proxied /api/ route rather than fppd's internal
        // :32322 port directly - that port isn't a documented interface.
        $gpiojson = json_decode(@file_get_contents('http://127.0.0.1/api/gpio'), true);
        foreach (is_array($gpiojson) ? $gpiojson : array() as $gpio) {
            if (isset($gpio['gpioLine']) && $gpio['gpioLine'] == (int)$curGpio &&
                isset($gpio['gpioChip']) && $gpio['gpioChip'] == $defaultGPIOChip) {
                WriteSettingToFile("ResetPin", $gpio['pin'], "fpp-vastfmt");
                break;
            }
        }
    }
?>

<div id="global" class="settings">
<?
PrintSettingGroup("VASTFMTHardware", "", "", 1, "fpp-vastfmt");
PrintSettingGroup("VASTFMTPlugin", "", "", 1, "fpp-vastfmt");
PrintSettingGroup("VASTFMTRadio", "", "", 1, "fpp-vastfmt");
PrintSettingGroup("VASTFMTRDS", "", "", 1, "fpp-vastfmt");
?>
</div>

<style>
/* FPP's own design-system tokens; they are redefined under
   [data-bs-theme='dark'] so this follows the theme instead of being one grey
   that is a compromise in both. Literals are fallbacks for older FPP. */
#vfmPsPreviewWrap { margin: 0.35rem 0 0 0; max-width: 100%; }
#vfmPsChunks { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
               font-size: 0.95rem; letter-spacing: 0.02em; }
.vfm-ps-chunk {
    display: inline-block;
    border: 1px solid var(--fpp-border, #888);
    border-radius: 3px;
    padding: 0.15rem 0.35rem;
    margin: 0.15rem 0.35rem 0.15rem 0;
    background: var(--fpp-bg-hover, rgba(127,127,127,0.12));
    white-space: pre;
}
.vfm-ps-space { opacity: 0.45; }
#vfmPsMeta { font-size: 0.85rem; opacity: 0.8; margin-top: 0.2rem; }
</style>

<div id="vfmPsPreviewWrap" style="display:none;">
  <div id="vfmPsChunks"></div>
  <div id="vfmPsMeta"></div>
</div>

<br />

<div id="VASTFMTStatus" class="settings">
<fieldset>
<legend>Transmitter Status</legend>
<p class="text-body">The antenna capacitor is searched automatically only while
<b>Antenna Tuning Capacitor</b> is 0. <b>Test Antenna Tuning</b> runs that search
once and reports what it would pick, then puts your configured value back - it
does not change the setting. It briefly re-tunes the transmitter, so avoid it
during a show.</p>
<div class="mb-3">
  <button type="button" class="buttons btn-success" id="vfmRetuneBtn" onclick="vfmRetune();">Test Antenna Tuning</button>
  <span id="vfmRetuneMsg" class="ms-2"></span>
</div>
<div class="table-responsive" style="max-width: 40rem;">
<table class="table table-sm">
  <tbody>
    <tr><th scope="row">Transmitter</th><td id="vfmState">&mdash;</td></tr>
    <tr><th scope="row">Connection</th><td id="vfmConn">&mdash;</td></tr>
    <tr><th scope="row">Frequency</th><td id="vfmFreq">&mdash;</td></tr>
    <tr><th scope="row">Power</th><td id="vfmPower">&mdash;</td></tr>
    <tr><th scope="row">Antenna cap</th><td id="vfmCap">&mdash;</td></tr>
    <tr><th scope="row">Antenna match</th><td id="vfmMatch">&mdash;</td></tr>
    <tr><th scope="row">Audio level</th><td id="vfmAsq">&mdash;</td></tr>
  </tbody>
</table>
</div>
</fieldset>
</div>

<script>
function vfmSetText(id, text, warn) {
    var el = document.getElementById(id);
    if (!el) return;
    el.textContent = text;
    el.classList.toggle('text-warning', !!warn);
}

// Station text goes out 8 characters at a time, so show it the way a receiver
// will: padded to 8-character screens, with spaces made visible.
// PrintSetting emits id= but no name=, and carries its own inline onChange
// that saves the setting - so look the field up by id and ADD a listener
// rather than replacing anything.
function vfmUpdatePsPreview() {
    var inp = document.getElementById('StationText');
    var wrap = document.getElementById('vfmPsPreviewWrap');
    if (!inp || !wrap) return;
    var txt = inp.value || '';
    var chunks = document.getElementById('vfmPsChunks');
    chunks.innerHTML = '';
    if (!txt.length) { wrap.style.display = 'none'; return; }
    wrap.style.display = '';
    for (var i = 0; i < txt.length; i += 8) {
        var part = txt.substr(i, 8);
        while (part.length < 8) part += ' ';
        var span = document.createElement('span');
        span.className = 'vfm-ps-chunk';
        for (var c = 0; c < part.length; c++) {
            var ch = document.createElement('span');
            if (part[c] === ' ') { ch.className = 'vfm-ps-space'; ch.textContent = '\u00b7'; }
            else { ch.textContent = part[c]; }
            span.appendChild(ch);
        }
        chunks.appendChild(span);
    }
    var n = Math.ceil(txt.length / 8);
    document.getElementById('vfmPsMeta').textContent =
        n + (n === 1 ? ' screen' : ' screens') + ', ' + txt.length + ' of 64 characters';
}

function vfmRefreshStatus() {
    $.ajax({ url: '/api/plugin-apis/vastfmt', dataType: 'json', cache: false,
      success: function(s) {
        vfmSetText('vfmState', s.state === 'ok' ? 'Running' : (s.state || 'unknown'),
                   s.state !== 'ok');
        vfmSetText('vfmConn', s.connection || '\u2014', false);
        if (s.state !== 'ok') {
            ['vfmFreq','vfmPower','vfmCap','vfmMatch','vfmAsq'].forEach(function(i){
                vfmSetText(i, '\u2014', false); });
            return;
        }
        vfmSetText('vfmFreq', s.frequency.toFixed(2) + ' MHz', false);
        vfmSetText('vfmPower', s.power + ' dB\u00b5V', false);
        vfmSetText('vfmCap', s.antCap + ' (' + s.antCapPf.toFixed(2) + ' pF)'
                   + (s.antCapAuto ? ' \u2014 automatic' : ' \u2014 set by hand'), false);
        vfmSetText('vfmMatch', s.matchOk ? 'ok' : 'no match found', !s.matchOk);
        vfmSetText('vfmAsq', s.asq || '\u2014', false);
      },
      error: function() { vfmSetText('vfmState', 'status unavailable', true); }
    });
}

function vfmRetune() {
    var btn = document.getElementById('vfmRetuneBtn');
    btn.disabled = true;
    vfmSetText('vfmRetuneMsg', 'testing\u2026', false);
    $.ajax({ url: '/api/plugin-apis/vastfmt/retune', type: 'POST', dataType: 'json', cache: false,
      success: function(r) {
        btn.disabled = false;
        if (!r.ok) { vfmSetText('vfmRetuneMsg', r.error || 'failed', true); return; }
        var msg = 'Automatic tuning picked ' + r.antCap + ' (' + r.antCapPf.toFixed(2) + ' pF). ';
        msg += r.matchOk ? 'That looks like a real match.'
                         : 'No match found - set the capacitor by hand.';
        if (r.restored) msg += ' Your setting of ' + r.restored + ' has been put back.';
        vfmSetText('vfmRetuneMsg', msg, !r.matchOk);
        vfmRefreshStatus();
      },
      error: function() { btn.disabled = false; vfmSetText('vfmRetuneMsg', 'request failed', true); }
    });
}

// The reset pin only means anything on the I2C path; showing it for a USB
// adapter just invites someone to set it. PrintSetting gives each row an
// id of "<setting>Row".
function vfmUpdateConnectionUI() {
    var conn = document.getElementById('Connection');
    var row = document.getElementById('ResetPinRow');
    if (!conn || !row) return;
    row.style.display = (conn.value === 'I2C') ? '' : 'none';
}

$(document).ready(function() {
    var conn = document.getElementById('Connection');
    if (conn) conn.addEventListener('change', vfmUpdateConnectionUI);
    vfmUpdateConnectionUI();

    var inp = document.getElementById('StationText');
    if (inp) {
        inp.addEventListener('input', vfmUpdatePsPreview);
        // Put the preview directly under the field it describes.
        var wrap = document.getElementById('vfmPsPreviewWrap');
        if (wrap && inp.parentNode) inp.parentNode.insertBefore(wrap, inp.nextSibling);
        vfmUpdatePsPreview();
    }
    vfmRefreshStatus();
    setInterval(vfmRefreshStatus, 1500);
});
</script>
