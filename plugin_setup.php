<?php
       
    $curGpio = ReadSettingFromFile("ResetPin", "fpp-vastfmt");

    $defaultGPIO = "P1-07";
    $defaultGPIOChip = 0;
    if ($settings['BeaglePlatform']) {        
        $defaultGPIO = "P9-22";
        if (strpos($settings['SubPlatform'], 'PocketBeagle') !== false) {
            $defaultGPIO = "P1-04";
        }
        $defaultGPIOChip = 3;
    }
    echo "<!-- " . $curGpio . "     " . $defaultGPIO . "  -->\n";
    // Go through Apache's proxied /api/ route rather than fppd's internal
    // :32322 port directly - that port isn't a documented interface and isn't
    // guaranteed to stay where it is.
    $data = file_get_contents('http://127.0.0.1/api/gpio');
    $gpiojson = json_decode($data, true);
    $gpioPins = Array();
    foreach(is_array($gpiojson) ? $gpiojson : Array() as $gpio) {
        $pn = $gpio['pin'] . ' (GPIO: ' . $gpio['gpioChip'] . '/' . $gpio['gpioLine'] . ')';
        $gpioPins[$pn] = $gpio['pin'];
        
        if ($curGpio == $gpio['pin'] || ($curGpio == $gpio["gpioLine"] && $gpio["gpioChip"] == $defaultGPIOChip)) {
            $defaultGPIO = $gpio['pin'];
            $pluginSettings["ResetPin"] = $defaultGPIO;
            WriteSettingToFile("ResetPin", $defaultGPIO, "fpp-vastfmt");
        }
    }
    echo "<!-- " . $curGpio . "     " . $defaultGPIO . "  -->\n";
?>

<script type="text/javascript">

function OnConnectionChanged() {
    var value = $('#Connection').val();
    if (value == "USB") {
        $('#ResetPinInfo').hide();
    } else {
        $('#ResetPinInfo').show();
    }
}
</script>


<div id="VASTFMTPluginhw" class="settings">
<fieldset>
<legend>VAST-FMT/Si4713 Hardware</legend>
<p>Connection: <?php PrintSettingSelect("Connection", "Connection", 0, 0, "USB", Array("USB"=>"USB", "I2C"=>"I2C"), "fpp-vastfmt", "OnConnectionChanged"); ?></p>
<p class="ResetPinInfo" id="ResetPinInfo">Reset GPIO: <?php PrintSettingSelect("ResetPin", "ResetPin", 0, 0, $defaultGPIO, $gpioPins, "fpp-vastfmt", ""); ?><br />
I2C connection requires a GPIO pin to reset/enable the Si4713.</p>
</fieldset>
</div>

<br />

<div id="VASTFMTPluginsettings" class="settings">
<fieldset>
<legend>VAST-FMT/Si4713 Plugin Settings</legend>
<p>Start at: <?php PrintSettingSelect("Start", "Start", 0, 0, "FPPDStart", Array("FPPD Start (default)"=>"FPPDStart", "Playlist Start"=>"PlaylistStart", "Never - RDS Only"=>"RDSOnly", "Never"=>"Never"), "fpp-vastfmt", ""); ?><br />
At Start, the hardware is reset, FM settings initialized, will broadcast any audio played, and send static RDS messages (if enabled).</p>
<p>Stop at: <?php PrintSettingSelect("Stop", "Stop", 0, 0, "Never", Array("Playlist Stop"=>"PlaylistStop", "Never (default)"=>"Never"), "fpp-vastfmt", ""); ?><br />
At Stop, the hardware is reset. Listeners will hear static.</p>
<p>Enable Volume Change Hack for Vast-FMT 212R: <?php PrintSettingCheckbox("EnableVolumeChangeHack", "EnableVolumeChangeHack", 0, 0, "1", "0", "fpp-vastfmt", ""); ?></p>
</fieldset>
</div>

<br />

<div id="VASTFMTsettings" class="settings">
<fieldset>
<legend>VAST-FMT/Si4713 FM Settings</legend>
<p>Frequency (76.00-108.00): <?php PrintSettingTextSaved("Frequency", 0, 0, 6, 6, "fpp-vastfmt", "100.10"); ?>MHz</p>
<p>Power (88-115, 116-120<sup>*</sup>): <?php PrintSettingTextSaved("Power", 0, 0, 3, 3, "fpp-vastfmt", "110"); ?>dB&mu;V
<br /><sup>*</sup>Can be set as high as 120dB&mu;V, but voltage accuracy above 115dB&mu;V is not guaranteed.</p>
<p>Preemphasis: <?php PrintSettingSelect("Preemphasis", "Preemphasis", 0, 0, "75us", Array("50&mu;s (Europe, Australia, Japan)"=>"50us", "75&mu;s (USA, default)"=>"75us"), "fpp-vastfmt", ""); ?></p>
<p>Antenna Tuning Capacitor (0=Auto, 1-191): <?php PrintSettingTextSaved("AntCap", 0, 0, 3, 3, "fpp-vastfmt", "0"); ?> * 0.25pF (If set to 0 and no FM signal detected, try a value around 50-80)</p>
<p>Enable Audio Limitter: <?php PrintSettingCheckbox("AudioLimitter", "AudioLimitter", 0, 0, "True", "False", "fpp-vastfmt", "", "True"); ?></p>
<p>Enable Audio Compression: <?php PrintSettingCheckbox("AudioCompression", "AudioCompression", 0, 0, "True", "False", "fpp-vastfmt", "", "True"); ?></p>
<p>Audio Compression Threshold (-64 - 0 db): <?php PrintSettingTextSaved("AudioCompressionThreshold", 0, 0, 3, 3, "fpp-vastfmt", "-15"); ?></p>
<p>Audio Gain (0 - 16): <?php PrintSettingTextSaved("AudioGain", 0, 0, 3, 3, "fpp-vastfmt", "5"); ?></p>
</fieldset>
</div>

<br />

<div id="VASTFMTRDSsettings" class="settings">
<fieldset>
<legend>VAST-FMT/Si4713 RDS Settings</legend>
<p>Enable RDS: <?php PrintSettingCheckbox("EnableRDS", "EnableRDS", 0, 0, "True", "False", "fpp-vastfmt", ""); ?></p>
<p>RDS Station - Sent 8 characters at a time.  Max of 64 characters.<br />
Station Text: <?php PrintSettingTextSaved("StationText", 0, 0, 64, 32, "fpp-vastfmt", "Merry   Christ- mas"); ?>

<br />

<p>RDS Text: <?php PrintSettingTextSaved("RDSTextText", 0, 0, 64, 32, "fpp-vastfmt", "[{Artist} - {Title}]"); ?>
<p>
Place {Artist} or {Title} where the media artist/title should be placed. Area's wrapped in brackets ( [] ) will not be output unless media is present.


<p>Program Type (PTY North America / Europe): <?php PrintSettingSelect("Pty", "Pty", 0, 0, 2,
Array(
"0 - None / None"=>0, 
"1 - News / News"=>1, 
"2 - Information / Current Affairs"=>2, 
"3 - Sport / Information"=>3, 
"4 - Talk / Sport"=>4, 
"5 - Rock / Education"=>5, 
"6 - Classic Rock / Drama"=>6, 
"7 - Adult Hits / Culture"=>7, 
"8 - Soft Rock / Science"=>8, 
"9 - Top 40 / Varied"=>9, 
"10 - Country / Pop"=>10, 
"11 - Oldies / Rock"=>11, 
"12 - Soft Music / Easy Listening"=>12, 
"13 - Nostalgia / Light Classical"=>13, 
"14 - Jazz / Serious Classical"=>14, 
"15 - Classical / Other Music"=>15, 
"16 - R&B / Weather"=>16, 
"17 - Soft R&B / Finance"=>17, 
"18 - Language / Childrens"=>18, 
"19 - Religious Music / Social Affairs"=>19, 
"20 - Religious Talk / Religion"=>20, 
"21 - Personality / Phone-In"=>21, 
"22 - Public / Travel"=>22, 
"23 - College / Leisure"=>23, 
"24 - --- / Jazz"=>24, 
"25 - --- / Country"=>25, 
"26 - --- / National Music"=>26, 
"27 - --- / Oldies"=>27, 
"28 - --- / Folk"=>28, 
"29 - Weather / Documentary"=>29), 
"fpp-vastfmt", ""); ?> - <a href="https://www.electronics-notes.com/articles/audio-video/broadcast-audio/rds-radio-data-system-pty-codes.php">Additional PTY information</a></p>
</fieldset>
</div>

<br />
<script>
OnConnectionChanged()
</script>

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
function vfmUpdatePsPreview() {
    var inp = document.getElementsByName('StationText')[0];
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

$(document).ready(function() {
    var inp = document.getElementsByName('StationText')[0];
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
