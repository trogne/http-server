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

const activeLayers = new Set(['motion','intervals']);
const voiceColors = ['#d9b86c','#83b8ad','#b49ac4','#d08f74','#7fa5ce','#b7bd78'];
function eventAt(events,time){return events.find(event=>!event.rest&&time>=event.start&&time<event.start+event.duration)||null;}
function analyzeFabric(){
  const voices=parseVoices(),timelines=voices.map(parseEvents),ends=timelines.flat().map(e=>e.start+e.duration),totalBeats=ends.length?Math.max(...ends):0,boundaries=[...new Set([0,totalBeats,...timelines.flatMap(events=>events.flatMap(e=>[e.start,e.start+e.duration]))])].sort((a,b)=>a-b),consonances=new Set([0,3,4,5,7,8,9]),perfect=new Set([0,7]),pairs=[];
  let verticalTotal=0,consonantTotal=0,activeSum=0,samples=0,crossings=0;
  for(let p=0;p<voices.length;p++)for(let q=p+1;q<voices.length;q++){
    const motion={contrary:0,parallel:0,similar:0,oblique:0},intervals={consonant:0,dissonant:0},links=[];let previous=null;
    boundaries.slice(0,-1).forEach((time,index)=>{const a=eventAt(timelines[p],time+.0001),b=eventAt(timelines[q],time+.0001);if(!a||!b){previous=null;return;}const ic=Math.abs(a.midi-b.midi)%12,consonant=consonances.has(ic);intervals[consonant?'consonant':'dissonant']++;verticalTotal++;if(consonant)consonantTotal++;if(a.midi<b.midi)crossings++;let kind='';if(previous&&(previous.a!==a.midi||previous.b!==b.midi)){const da=Math.sign(a.midi-previous.a),db=Math.sign(b.midi-previous.b);if(!da||!db)kind='oblique';else if(da!==db)kind='contrary';else if(perfect.has(ic)&&perfect.has(previous.ic)&&ic===previous.ic)kind='parallel';else kind='similar';motion[kind]++;}links.push({time,end:boundaries[index+1],a,b,ic,consonant,kind});previous={a:a.midi,b:b.midi,ic};});
    const onsetsA=timelines[p].filter(e=>!e.rest).map(e=>e.start),onsetsB=timelines[q].filter(e=>!e.rest).map(e=>e.start),union=new Set([...onsetsA,...onsetsB]),shared=[...union].filter(t=>onsetsA.includes(t)&&onsetsB.includes(t)).length,independence=union.size?Math.round((1-shared/union.size)*100):0,signature=events=>events.filter(e=>!e.rest).slice(0,4).map((e,i,a)=>i?e.midi-a[i-1].midi:null).slice(1).join(','),imitation=signature(timelines[p])&&signature(timelines[p])===signature(timelines[q]);pairs.push({p,q,motion,intervals,links,independence,imitation});
  }
  for(let time=0;time<totalBeats;time+=.5){activeSum+=timelines.filter(events=>eventAt(events,time+.001)).length;samples++;}
  const entrances=timelines.flatMap((events,index)=>{const first=events.find(e=>!e.rest);return first?[{time:first.start,voice:index}]:[];}),cadenceTimes=boundaries.slice(1,-1).filter(time=>{const before=timelines.map(e=>eventAt(e,time-.001)),after=timelines.map(e=>eventAt(e,time+.001));return before.filter((e,i)=>e&&after[i]&&e.midi!==after[i].midi).length>=Math.max(2,Math.ceil(voices.length*.66));});
  return{voices,timelines,totalBeats,pairs,entrances,cadenceTimes,verticalTotal,consonantTotal,crossings,density:samples?activeSum/samples:0};
}
function midiName(midi){const names=['C','C♯','D','E♭','E','F','F♯','G','A♭','A','B♭','B'];return names[(midi%12+12)%12]+(Math.floor(midi/12)-1);}
function renderTexture() {
  const fabric=analyzeFabric(),{voices,timelines,totalBeats}=fabric,selected=Number(voiceSelect.value||0);
  const sounded = timelines.map(events => events.filter(event => !event.rest));
  const all = sounded.flat().map(event => event.midi); if (!all.length) return;
  const svg=$('contour'),width=1000,height=430,left=58,right=24,top=32,bottom=44;
  const min = Math.min(...all), max = Math.max(...all), span = Math.max(max - min, 1);
  const x=time=>left+time*(width-left-right)/Math.max(totalBeats,1),y=midi=>height-bottom-(midi-min)*(height-top-bottom)/span,point=event=>({x:x(event.start),y:y(event.midi)}),beatLines=Array.from({length:Math.floor(totalBeats)+1},(_,i)=>`<line x1="${x(i)}" y1="${top}" x2="${x(i)}" y2="${height-bottom}"/><text x="${x(i)+4}" y="${height-15}">${i+1}</text>`).join(''),registerLines=[min,Math.round((min+max)/2),max].map(m=>`<line x1="${left}" y1="${y(m)}" x2="${width-right}" y2="${y(m)}"/><text x="8" y="${y(m)+4}">${midiName(m)}</text>`).join('');
  const links=fabric.pairs.filter(pair=>pair.p===selected||pair.q===selected).flatMap(pair=>pair.links.map(link=>({pair,link}))).map(({pair,link})=>{const mid=(link.time+link.end)/2,interval=activeLayers.has('intervals')?`<line class="relation-link ${link.consonant?'consonant':'tension'}" x1="${x(mid)-2}" y1="${y(link.a.midi)}" x2="${x(mid)-2}" y2="${y(link.b.midi)}"><title>${voices[pair.p].name} / ${voices[pair.q].name}: ${link.consonant?'consonance':'dissonance'}</title></line>`:'',motion=activeLayers.has('motion')&&link.kind?`<line class="relation-link motion-link ${link.kind}" x1="${x(mid)+2}" y1="${y(link.a.midi)}" x2="${x(mid)+2}" y2="${y(link.b.midi)}"><title>${voices[pair.p].name} / ${voices[pair.q].name}: ${link.kind} motion</title></line>`:'';return interval+motion;}).join('');
  const structure=activeLayers.has('structure')?[...fabric.entrances.map(e=>`<g class="structure-mark"><line x1="${x(e.time)}" y1="${top}" x2="${x(e.time)}" y2="${height-bottom}"/><text x="${x(e.time)+6}" y="${top+14}">${escapeText(voices[e.voice].name)} enters</text></g>`),...fabric.cadenceTimes.slice(-2).map(t=>`<g class="structure-mark cadence"><line x1="${x(t)}" y1="${top}" x2="${x(t)}" y2="${height-bottom}"/><text x="${x(t)+6}" y="${top+30}">convergence</text></g>`)].join(''):'';
  const lines = sounded.map((events, voiceIndex) => {
    const points=events.map(point),color=voiceColors[voiceIndex%voiceColors.length],delay=voiceIndex*.12,path=events.map((event,index)=>{const start=point(event),next=events[index+1],connector=next&&Math.abs(event.start+event.duration-next.start)<.001?` L ${x(next.start)} ${y(next.midi)}`:'';return`M ${start.x} ${start.y} H ${x(event.start+event.duration)}${connector}`;}).join(' ');
    const traced=voiceIndex===selected;
    return `<g class="texture-voice${traced?' is-traced':''}" style="--voice:${color};--delay:${delay}s"><path class="texture-line" pathLength="1" d="${path}"/>`+points.map((p,index)=>`<circle class="texture-point" style="--point-delay:${delay+index*.018}s" cx="${p.x}" cy="${p.y}" r="${traced?4.8:3.5}"><title>${escapeText(voices[voiceIndex].name)} · ${escapeText(events[index].note)} · beat ${events[index].start+1}</title></circle>`).join('')+'</g>';
  }).join('');
  svg.innerHTML=`<g class="grid beat-grid">${beatLines}</g><g class="grid register-grid">${registerLines}</g>${structure}<g class="relationships">${links}</g>${lines}`;
  svg.setAttribute('aria-label',`${voices.length} contrapuntal voices shown with equal emphasis across ${totalBeats} beats`);
  $('fabric-label').textContent=`Tracing ${voices[selected]?.name||'voice'} ↔ ${Math.max(voices.length-1,0)} connection${voices.length===2?'':'s'} · ${totalBeats} beats`;
  $('voice-legend').innerHTML=voices.map((voice,index)=>`<button type="button" class="${index===selected?'traced':''}" data-voice="${index}"><i style="--voice:${voiceColors[index%voiceColors.length]}"></i>${escapeText(voice.name)}<small>${index===selected?'connections traced':'trace connections'}</small></button>`).join('');
  $('voice-legend').querySelectorAll('button').forEach(button=>button.addEventListener('click',()=>{voiceSelect.value=button.dataset.voice;renderTexture();}));
}
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

function renderAnalysis(data){const fabric=analyzeFabric(),totals={contrary:0,parallel:0,similar:0,oblique:0};fabric.pairs.forEach(pair=>Object.keys(totals).forEach(key=>totals[key]+=pair.motion[key]));const pairCount=fabric.voices.length*(fabric.voices.length-1)/2,stability=fabric.verticalTotal?Math.round(fabric.consonantTotal/fabric.verticalTotal*100):0;$('m-voices').textContent=`${fabric.voices.length} / ${pairCount}`;$('m-motion').textContent=`${totals.contrary} · ${totals.parallel} · ${totals.similar} · ${totals.oblique}`;$('m-stability').textContent=`${stability}%`;$('m-density').textContent=fabric.density.toFixed(1);$('fabric-label').textContent=`${fabric.voices.length} equal voices · ${fabric.totalBeats} beats · ${pairCount} pair${pairCount===1?'':'s'}`;renderTexture();$('pairwise-list').innerHTML=fabric.pairs.map(pair=>{const m=pair.motion,stable=pair.intervals.consonant+pair.intervals.dissonant?Math.round(pair.intervals.consonant/(pair.intervals.consonant+pair.intervals.dissonant)*100):0;return`<div class="pair-row"><div><i style="--a:${voiceColors[pair.p%voiceColors.length]};--b:${voiceColors[pair.q%voiceColors.length]}"></i><strong>${escapeText(fabric.voices[pair.p].name)} ↔ ${escapeText(fabric.voices[pair.q].name)}</strong><small>${stable}% consonant · ${pair.independence}% rhythmic independence${pair.imitation?' · opening imitation':''}</small></div><dl><span><b>${m.contrary}</b> contrary</span><span><b>${m.parallel}</b> parallel</span><span><b>${m.similar}</b> similar</span><span><b>${m.oblique}</b> oblique</span></dl></div>`;}).join('')||'<p class="empty">Add a second voice to begin a relational reading.</p>';const resolutions=fabric.pairs.flatMap(pair=>pair.links.map((link,i)=>!link.consonant&&pair.links[i+1]?.consonant?{time:pair.links[i+1].time,label:'Tension resolves',copy:`${fabric.voices[pair.p].name} and ${fabric.voices[pair.q].name} move from dissonance into consonance.`}:null).filter(Boolean)).slice(0,4),events=[...fabric.entrances.map(e=>({time:e.time,label:`${fabric.voices[e.voice].name} enters`,copy:e.time?'Staggered entrance expands the active texture.':'Present at the opening of the texture.'})),...resolutions,...fabric.cadenceTimes.slice(-3).map(t=>({time:t,label:'Coordinated change',copy:'Multiple voices redirect together: test this point for convergence or structural cadence.'}))].sort((a,b)=>a.time-b.time);$('event-list').innerHTML=events.map(e=>`<div class="event-row"><b>${e.time.toFixed(1)}</b><p><strong>${escapeText(e.label)}</strong><span>${escapeText(e.copy)}</span></p></div>`).join('')||'<p class="empty">No shared structural events detected.</p>';const dominant=Object.entries(totals).sort((a,b)=>b[1]-a[1])[0]?.[0]||'independent';$('insight-title').textContent=`${dominant[0].toUpperCase()+dominant.slice(1)} motion shapes the exchange`;$('insight-copy').textContent=`Across every pair, the texture moves through ${totals.contrary} contrary, ${totals.parallel} parallel, ${totals.similar} similar, and ${totals.oblique} oblique changes. ${stability}% of sampled verticalities are provisionally consonant; ${fabric.crossings} registral crossing${fabric.crossings===1?' is':'s are'} visible. Listen for preparation and resolution around marked tensions, and for imitation around staggered entrances.`;$('saved-badge').textContent=data.id?'✓ Saved to Neon archive':'Analyzed locally';results.classList.remove('hidden');results.scrollIntoView({behavior:'smooth',block:'start'});}

form.addEventListener('submit',async event=>{event.preventDefault();error.textContent='';const button=form.querySelector('.analyze'),old=button.innerHTML;button.disabled=true;button.textContent='Reading the fabric…';try{const voices=syncVoices();validateScore(voices);const focus=voices[Number(voiceSelect.value||0)];if(!focus)throw new Error('Enter at least one voice using “Name: C4 D4 E4”.');const focusNotes=parseEvents(focus,true).filter(e=>!e.rest).map(e=>e.note).join(' ');if(!focusNotes)throw new Error('The traced voice needs at least one sounding note.');const response=await fetch('/api/analyze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({notes:focusNotes,score:notes.value})});const data=await response.json();if(!response.ok)throw new Error(data.error||'Analysis failed');renderAnalysis(data);await loadHistory();}catch(cause){error.textContent=cause.message;}finally{button.disabled=false;button.innerHTML=old;}});

function escapeText(text){const span=document.createElement('span');span.textContent=text;return span.innerHTML;}
async function loadHistory(){const history=$('history');try{const response=await fetch('/api/analyses',{cache:'no-store'}),data=await response.json();if(!response.ok)throw new Error();if(!data.analyses.length){history.innerHTML='<p class="empty">Your analyzed subjects will appear here.</p>';return;}history.innerHTML=data.analyses.map(item=>{const score=item.score||`Voice I: ${item.notes}`,voiceNames=score.split(/\n+/).map(line=>line.match(/^([^:]+):/)?.[1].trim()).filter(Boolean),title=voiceNames.length>1?voiceNames.join(' · '):item.notes;return`<button class="history-item" type="button"><span class="history-id">${String(item.id).padStart(3,'0')}</span><span><strong>${escapeText(title)}</strong><small>${item.created_at} UTC · ${voiceNames.length||1} voice${voiceNames.length===1?'':'s'} · ${item.summary.note_count} focus notes · ${item.summary.range_semitones} semitones</small></span><b>Revisit score →</b></button>`;}).join('');history.querySelectorAll('.history-item').forEach((element,index)=>element.addEventListener('click',()=>{const item=data.analyses[index];notes.value=item.score||`Voice I: ${item.notes}`;syncVoices();$('workbench').scrollIntoView({behavior:'smooth',block:'start'});notes.focus({preventScroll:true});}));}catch{history.innerHTML='<p class="empty">Archive unavailable. Your server may still be waking up.</p>';}}

$('refresh').addEventListener('click',loadHistory);notes.addEventListener('input',syncVoices);notes.addEventListener('keydown',event=>{if((event.ctrlKey||event.metaKey)&&event.key==='Enter')form.requestSubmit();});voiceSelect.addEventListener('change',()=>{stopAudio();if(!results.classList.contains('hidden'))renderTexture();});tempo.addEventListener('input',()=>{$('tempo-value').textContent=`${tempo.value} BPM`;});$('play-texture').addEventListener('click',()=>playVoices(false));$('play-focus').addEventListener('click',()=>playVoices(true));$('stop-audio').addEventListener('click',stopAudio);document.querySelectorAll('[data-layer]').forEach(button=>button.addEventListener('click',()=>{const layer=button.dataset.layer;if(activeLayers.has(layer))activeLayers.delete(layer);else activeLayers.add(layer);button.classList.toggle('active',activeLayers.has(layer));button.setAttribute('aria-pressed',String(activeLayers.has(layer)));if(!results.classList.contains('hidden'))renderTexture();}));if(/^Archived voice\s*:/i.test(notes.value))notes.value=notes.value.replace(/^Archived voice\s*:/i,'Voice I:');syncVoices();loadHistory();

if('IntersectionObserver'in window){const navLinks=[...document.querySelectorAll('nav a[href^="#"]')],navObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(!entry.isIntersecting)return;navLinks.forEach(link=>link.toggleAttribute('aria-current',link.getAttribute('href')===`#${entry.target.id}`));}),{rootMargin:'-25% 0px -65%'});navLinks.forEach(link=>{const section=document.querySelector(link.getAttribute('href'));if(section)navObserver.observe(section);});const revealObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){entry.target.classList.add('is-visible');revealObserver.unobserve(entry.target);}}),{rootMargin:'0px 0px -8%',threshold:.08});document.querySelectorAll('.method-intro,.method-story,.method-image,.practice-steps,.method-result,.invitation>*,.studio-top,#analysis-form,.section-heading,.metric-grid,.analysis-grid,.insight,.history').forEach(element=>{element.classList.add('reveal');revealObserver.observe(element);});}
const progressBar=document.querySelector('.reading-progress i');const updateProgress=()=>{const distance=document.documentElement.scrollHeight-innerHeight;progressBar.style.transform=`scaleX(${distance>0?Math.min(scrollY/distance,1):0})`;};addEventListener('scroll',updateProgress,{passive:true});addEventListener('resize',updateProgress);updateProgress();
