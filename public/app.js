const $ = id => document.getElementById(id);
const form = $('analysis-form'), notes = $('notes'), error = $('error'), results = $('results');
const voiceSelect = $('voice-select'), tempo = $('tempo');
const sampleButtons = document.querySelectorAll('[data-notes]');
let audioContext, activeSources = [], playbackTimer;

sampleButtons.forEach(button => button.addEventListener('click', () => {
  stopAudio(); notes.value = button.dataset.notes; syncVoices(); notes.focus();
  button.closest('.example-list').querySelectorAll('button').forEach(item => item.classList.toggle('selected', item === button));
}));

function parseVoices() {
  return notes.value.split(/\n+/).map((line, index) => {
    const match = line.trim().match(/^([^:]+):\s*(.+)$/);
    return match ? {name:match[1].trim(), notes:match[2].trim()} : line.trim() ? {name:`Voice ${index + 1}`, notes:line.trim()} : null;
  }).filter(Boolean);
}

function noteToMidi(note) {
  const match = note.match(/^([A-Ga-g])([#b]?)(-?\d)$/);
  if (!match) return NaN;
  const base = {C:0,D:2,E:4,F:5,G:7,A:9,B:11}[match[1].toUpperCase()];
  return (Number(match[3]) + 1) * 12 + base + (match[2] === '#' ? 1 : match[2] === 'b' ? -1 : 0);
}

function parseEvents(voice, strict = false) {
  let cursor = 0;
  return voice.notes.split(/[\s,]+/).filter(Boolean).map(token => {
    const parts = token.split('/');
    if (parts.length > 2) { if (strict) throw new Error(`${voice.name}: invalid token “${token}”.`); return null; }
    const duration = Number(parts[1] || 1);
    if (!Number.isFinite(duration) || duration <= 0 || duration > 16) { if (strict) throw new Error(`${voice.name}: invalid duration in “${token}”.`); return null; }
    const rest = parts[0].toUpperCase() === 'R';
    const midi = rest ? null : noteToMidi(parts[0]);
    if (!rest && !Number.isFinite(midi)) {
      if (strict) {
        const hint = /^[A-Ga-g][#b]?$/.test(parts[0]) ? ` Add an octave, for example ${parts[0]}4.` : ' Use notes such as C4, C#4, Db4, or R.';
        throw new Error(`${voice.name}: invalid note “${parts[0]}”.${hint}`);
      }
      return null;
    }
    const event = {note:parts[0], midi, start:cursor, duration, rest}; cursor += duration; return event;
  }).filter(Boolean);
}

function validateScore(voices) { voices.forEach(voice => parseEvents(voice, true)); }

function syncVoices() {
  const voices = parseVoices(), selected = voiceSelect.value;
  voiceSelect.innerHTML = voices.map((voice,index) => `<option value="${index}">${escapeText(voice.name)}</option>`).join('');
  if (voices[selected]) voiceSelect.value = selected;
  return voices;
}

function renderTexture() {
  const voices = parseVoices(), selected = Number(voiceSelect.value || 0);
  const timelines = voices.map(voice => parseEvents(voice));
  const sounded = timelines.map(events => events.filter(event => !event.rest));
  const all = sounded.flat().map(event => event.midi); if (!all.length) return;
  const svg = $('contour'), width = 760, height = 250, pad = 32;
  const min = Math.min(...all), max = Math.max(...all), span = Math.max(max - min, 1);
  const totalBeats = Math.max(...timelines.flat().map(event => event.start + event.duration));
  const point = event => ({x:pad+event.start*(width-pad*2)/Math.max(totalBeats,1), y:height-pad-(event.midi-min)*(height-pad*2)/span});
  const colors = ['#caa45d','#91a8a0','#a68ca8','#a98772','#8296b0'];
  const horizontalGrid = [0,1,2,3].map(i => `<line x1="${pad}" y1="${pad+i*(height-pad*2)/3}" x2="${width-pad}" y2="${pad+i*(height-pad*2)/3}"/>`).join('');
  const verticalGrid = [0,1,2,3,4].map(i => `<line x1="${pad+i*(width-pad*2)/4}" y1="${pad}" x2="${pad+i*(width-pad*2)/4}" y2="${height-pad}"/>`).join('');
  const lines = sounded.map((events, voiceIndex) => {
    const points=events.map(point), color=colors[voiceIndex%colors.length], delay=voiceIndex*.12, focused=voiceIndex===selected;
    return `<g class="texture-voice${focused?' is-focus':''}" style="--voice:${color};--delay:${delay}s"><polyline class="texture-line" pathLength="1" points="${points.map(p=>`${p.x},${p.y}`).join(' ')}"/>` + points.map((p,index)=>`<circle class="texture-point" style="--point-delay:${delay+index*.018}s" cx="${p.x}" cy="${p.y}" r="${focused?5:3.5}"><title>${escapeText(voices[voiceIndex].name)} · ${escapeText(events[index].note)} · beat ${events[index].start+1}</title></circle>`).join('') + '</g>';
  }).join('');
  svg.innerHTML = `<g class="grid vertical">${verticalGrid}</g><g class="grid">${horizontalGrid}</g>${lines}`;
  svg.setAttribute('aria-label', `${voices.length} contrapuntal voices with ${voices[selected]?.name || 'the selected voice'} emphasized across ${totalBeats} beats`);
  $('voice-legend').innerHTML = voices.map((voice,index) => `<span class="${index===selected?'is-focus':''}"><i style="--voice:${colors[index%colors.length]}"></i>${escapeText(voice.name)}${index===selected?' · focus':''}</span>`).join('');
}

function setBar(id,count,total){$(id+'-bar').style.width=`${total?Math.max(4,count/total*100):0}%`;$(id+'-value').textContent=count;}
function stopAudio(){activeSources.forEach(source=>{try{source.stop();}catch{}});activeSources=[];clearTimeout(playbackTimer);document.querySelectorAll('.transport button').forEach(button=>{button.classList.remove('playing');button.setAttribute('aria-pressed','false');});}

async function playVoices(focusOnly=false){
  stopAudio(); error.textContent='';
  try {
    const voices=parseVoices(), selected=Number(voiceSelect.value||0); if(!voices.length) throw new Error('Enter at least one labeled voice.'); validateScore(voices);
    audioContext ||= new (window.AudioContext||window.webkitAudioContext)(); await audioContext.resume();
    const beat=60/Number(tempo.value), start=audioContext.currentTime+.08, master=audioContext.createGain(), compressor=audioContext.createDynamicsCompressor();
    master.gain.value=.82; compressor.threshold.value=-18; compressor.knee.value=16; compressor.ratio.value=3; compressor.attack.value=.012; compressor.release.value=.22; master.connect(compressor).connect(audioContext.destination);
    const textureLevel=Math.min(.055,.095/Math.sqrt(Math.max(voices.length,1))); let latestRelease=start;
    voices.forEach((voice,voiceIndex)=>{ if(focusOnly&&voiceIndex!==selected)return; parseEvents(voice,true).forEach(event=>{ if(event.rest)return; const oscillator=audioContext.createOscillator(),gain=audioContext.createGain(); oscillator.type=['triangle','sine','triangle'][voiceIndex%3]; oscillator.detune.value=(voiceIndex-(voices.length-1)/2)*2.5; oscillator.frequency.value=440*Math.pow(2,(event.midi-69)/12); const onset=start+event.start*beat,release=onset+event.duration*beat*.92; latestRelease=Math.max(latestRelease,release); const level=focusOnly?.13:(voiceIndex===selected?textureLevel*2.25:textureLevel*.72); gain.gain.setValueAtTime(.0001,onset);gain.gain.exponentialRampToValueAtTime(level,onset+.025);gain.gain.setValueAtTime(level,Math.max(onset+.03,release-.08));gain.gain.exponentialRampToValueAtTime(.0001,release);oscillator.connect(gain);if(audioContext.createStereoPanner&&voices.length>1){const panner=audioContext.createStereoPanner();panner.pan.value=-.58+voiceIndex*1.16/(voices.length-1);gain.connect(panner).connect(master);}else gain.connect(master);oscillator.start(onset);oscillator.stop(release+.02);activeSources.push(oscillator);});});
    const activeButton=$(focusOnly?'play-focus':'play-texture');activeButton.classList.add('playing');activeButton.setAttribute('aria-pressed','true');playbackTimer=setTimeout(stopAudio,Math.max(0,latestRelease-audioContext.currentTime+.12)*1000);
  } catch(cause){error.textContent=cause.message;}
}

function describeTexture(){const voices=parseVoices(),rows=voices.map(v=>parseEvents(v));let contrary=0,similar=0,oblique=0,consonant=0,sonorities=0;for(let v=0;v<rows.length-1;v++){const end=Math.max(...rows[v].concat(rows[v+1]).map(e=>e.start+e.duration));let previousA=null,previousB=null;for(let time=0;time<end;time+=.5){const a=rows[v].find(e=>!e.rest&&time>=e.start&&time<e.start+e.duration),b=rows[v+1].find(e=>!e.rest&&time>=e.start&&time<e.start+e.duration);if(!a||!b){previousA=null;previousB=null;continue;}if([0,3,4,5,7,8,9].includes(Math.abs(a.midi-b.midi)%12))consonant++;sonorities++;if(previousA===null){previousA=a.midi;previousB=b.midi;continue;}const da=Math.sign(a.midi-previousA),db=Math.sign(b.midi-previousB);if(!da||!db)oblique++;else if(da===db)similar++;else contrary++;previousA=a.midi;previousB=b.midi;}}if(voices.length<2)return'Add another labeled voice to reveal contrapuntal relationships.';const dominant=contrary>=similar&&contrary>=oblique?'contrary':similar>=oblique?'similar':'oblique';return`Across ${voices.length} voices, ${dominant} motion is most frequent in adjacent pairs. Approximately ${sonorities?Math.round(consonant/sonorities*100):0}% of sampled vertical intervals are treated as consonant in this simplified reading.`;}

function renderAnalysis(data){$('m-notes').textContent=data.note_count;$('m-range').textContent=data.range_semitones;$('m-classes').textContent=data.distinct_pitch_classes;$('m-center').textContent=data.tonal_center;$('contour-label').textContent=`${voiceSelect.options[voiceSelect.selectedIndex]?.text||'Focus voice'} · ${data.contour}`;$('climax-label').textContent=`Climax · note ${data.climax_position}`;$('avg-motion').textContent=`${data.average_motion.toFixed(2)} semitones`;$('direction').textContent=`↑ ${data.ascending} · ↓ ${data.descending}`;const motions=data.steps+data.leaps+data.repeated;setBar('steps',data.steps,motions);setBar('leaps',data.leaps,motions);setBar('repeat',data.repeated,motions);renderTexture();const stepRatio=motions?data.steps/motions:0;$('insight-title').textContent=stepRatio>=.6?'A vocal, conjunct melodic design':data.leaps>data.steps?'An energized, disjunct subject':'A balanced interval design';$('insight-copy').textContent=`The focused line spans ${data.range_semitones} semitones and reaches its high point at note ${data.climax_position}. ${data.steps} stepwise motions create continuity while ${data.leaps} leap${data.leaps===1?'':'s'} add${data.leaps===1?'s':''} structural energy. ${describeTexture()} Treat these measurements as listening prompts, not a definitive harmonic analysis.`;$('saved-badge').textContent=data.id?'✓ Saved to Neon archive':'Analyzed locally';results.classList.remove('hidden');results.scrollIntoView({behavior:'smooth',block:'start'});}

form.addEventListener('submit',async event=>{event.preventDefault();error.textContent='';const button=form.querySelector('.analyze'),old=button.innerHTML;button.disabled=true;button.textContent='Reading the subject…';try{const voices=syncVoices();validateScore(voices);const focus=voices[Number(voiceSelect.value||0)];if(!focus)throw new Error('Enter at least one voice using “Name: C4 D4 E4”.');const focusNotes=parseEvents(focus,true).filter(e=>!e.rest).map(e=>e.note).join(' ');if(!focusNotes)throw new Error('The focus voice needs at least one sounding note.');const response=await fetch('/api/analyze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({notes:focusNotes,score:notes.value})});const data=await response.json();if(!response.ok)throw new Error(data.error||'Analysis failed');renderAnalysis(data);await loadHistory();}catch(cause){error.textContent=cause.message;}finally{button.disabled=false;button.innerHTML=old;}});

function escapeText(text){const span=document.createElement('span');span.textContent=text;return span.innerHTML;}
async function loadHistory(){const history=$('history');try{const response=await fetch('/api/analyses',{cache:'no-store'}),data=await response.json();if(!response.ok)throw new Error();if(!data.analyses.length){history.innerHTML='<p class="empty">Your analyzed subjects will appear here.</p>';return;}history.innerHTML=data.analyses.map(item=>{const score=item.score||`Voice I: ${item.notes}`,voiceNames=score.split(/\n+/).map(line=>line.match(/^([^:]+):/)?.[1].trim()).filter(Boolean),title=voiceNames.length>1?voiceNames.join(' · '):item.notes;return`<button class="history-item" type="button"><span class="history-id">${String(item.id).padStart(3,'0')}</span><span><strong>${escapeText(title)}</strong><small>${item.created_at} UTC · ${voiceNames.length||1} voice${voiceNames.length===1?'':'s'} · ${item.summary.note_count} focus notes · ${item.summary.range_semitones} semitones</small></span><b>Revisit score →</b></button>`;}).join('');history.querySelectorAll('.history-item').forEach((element,index)=>element.addEventListener('click',()=>{const item=data.analyses[index];notes.value=item.score||`Voice I: ${item.notes}`;syncVoices();$('workbench').scrollIntoView({behavior:'smooth',block:'start'});notes.focus({preventScroll:true});}));}catch{history.innerHTML='<p class="empty">Archive unavailable. Your server may still be waking up.</p>';}}

$('refresh').addEventListener('click',loadHistory);notes.addEventListener('input',syncVoices);notes.addEventListener('keydown',event=>{if((event.ctrlKey||event.metaKey)&&event.key==='Enter')form.requestSubmit();});voiceSelect.addEventListener('change',()=>{stopAudio();renderTexture();});tempo.addEventListener('input',()=>{$('tempo-value').textContent=`${tempo.value} BPM`;});$('play-texture').addEventListener('click',()=>playVoices(false));$('play-focus').addEventListener('click',()=>playVoices(true));$('stop-audio').addEventListener('click',stopAudio);if(/^Archived voice\s*:/i.test(notes.value))notes.value=notes.value.replace(/^Archived voice\s*:/i,'Voice I:');syncVoices();loadHistory();

if('IntersectionObserver'in window){const navLinks=[...document.querySelectorAll('nav a[href^="#"]')],navObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(!entry.isIntersecting)return;navLinks.forEach(link=>link.toggleAttribute('aria-current',link.getAttribute('href')===`#${entry.target.id}`));}),{rootMargin:'-25% 0px -65%'});navLinks.forEach(link=>{const section=document.querySelector(link.getAttribute('href'));if(section)navObserver.observe(section);});const revealObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){entry.target.classList.add('is-visible');revealObserver.unobserve(entry.target);}}),{rootMargin:'0px 0px -8%',threshold:.08});document.querySelectorAll('.method-intro,.method-story,.method-image,.practice-steps,.method-result,.invitation>*,.studio-top,#analysis-form,.section-heading,.metric-grid,.analysis-grid,.insight,.history').forEach(element=>{element.classList.add('reveal');revealObserver.observe(element);});}
const progressBar=document.querySelector('.reading-progress i');const updateProgress=()=>{const distance=document.documentElement.scrollHeight-innerHeight;progressBar.style.transform=`scaleX(${distance>0?Math.min(scrollY/distance,1):0})`;};addEventListener('scroll',updateProgress,{passive:true});addEventListener('resize',updateProgress);updateProgress();