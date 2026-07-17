const $ = id => document.getElementById(id);
const form = $('analysis-form'), notes = $('notes'), error = $('error'), results = $('results');
const voiceSelect = $('voice-select'), tempo = $('tempo');
const sampleButtons = document.querySelectorAll('[data-notes]');
let audioContext, activeSources = [], playbackTimer, latestReading = null;

sampleButtons.forEach(button => button.addEventListener('click', () => {
  stopAudio(); notes.value = button.dataset.notes; syncVoices(); if(!results.classList.contains('hidden')){results.classList.add('is-stale');$('saved-badge').textContent='Score changed · analyze again';} $('notation-staff')?.focus();
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
  if ($('notation-staff')) renderScoreEditor();
  return voices;
}

const notationState = {voice:0,kind:'note',duration:1,accidental:'natural',timeSignature:'4/4',measures:4};
const pitchLetters = ['C','D','E','F','G','A','B'];
const pitchIndex = (letter,octave) => octave * 7 + pitchLetters.indexOf(letter);
const pitchFromIndex = index => `${pitchLetters[(index % 7 + 7) % 7]}${Math.floor(index / 7)}`;
const durationLabel = duration => ({'.5':'eighth','0.5':'eighth','1':'quarter','2':'half','4':'whole'}[String(duration)] || `${duration}-beat`);
const meterParts = () => notationState.timeSignature.split('/').map(Number);
const measureBeats = () => { const [count,value]=meterParts(); return count*4/value; };
const meterPulse = () => meterParts()[1]===8?'Eighth-note pulse':'Quarter-note pulse';

function setScoreSource(voices) {
  notes.value = voices.map(voice => `${voice.name}: ${voice.notes || 'R/4'}`).join('\n');
  syncVoices(); markResultsStale();
}

function notationEventSvg(event,x,y,staffTop,staffBottom,options={}) {
  const duration=event.duration,up=options.stemUp??y>=(staffTop+staffBottom)/2,stemX=up?x+7:x-7,stemEnd=options.stemEnd??(up?y-42:y+42);
  if(event.rest){
    const rests={'.5':['𝄾','eighth',2],1:['𝄽','quarter',2],2:['𝄼','half',5],4:['𝄻','whole',0]},[glyph,label,offset]=rests[String(duration)]||rests[1];
    return `<text class="rest-glyph rest-${label}" x="${x}" y="${(staffTop+staffBottom)/2+offset}" aria-label="${label} rest">${glyph}</text>`;
  }
  const ledger=[];for(let ly=staffBottom+18;ly<=y+2;ly+=18)ledger.push(`<line x1="${x-14}" x2="${x+14}" y1="${ly}" y2="${ly}"/>`);for(let ly=staffTop-18;ly>=y-2;ly-=18)ledger.push(`<line x1="${x-14}" x2="${x+14}" y1="${ly}" y2="${ly}"/>`);
  const accidental=event.note.includes('#')?'♯':event.note.includes('b')?'♭':'';
  const open=duration>=2,head=open?`<g class="open-note-head" transform="rotate(-18 ${x} ${y})"><ellipse cx="${x}" cy="${y}" rx="9.5" ry="6.8"/><ellipse class="note-cutout" cx="${x}" cy="${y}" rx="5.4" ry="3.5"/></g>`:`<ellipse cx="${x}" cy="${y}" rx="9" ry="6.5" transform="rotate(-18 ${x} ${y})" class="note-head"/>`;
  const stem=duration===4?'':`<line class="note-stem" x1="${stemX}" y1="${y}" x2="${stemX}" y2="${stemEnd}"/>`;
  const flag=duration===.5&&!options.beamed?`<path class="note-flag" d="M${stemX} ${stemEnd}q20 ${up?8:-8} 12 ${up?25:-25}"/>`:'';
  return `<g class="engraved note-mark" aria-label="${event.note}, ${durationLabel(duration)} note">${ledger.join('')}${accidental?`<text class="score-accidental" x="${x-22}" y="${y+7}">${accidental}</text>`:''}${stem}${flag}${head}</g>`;
}

function renderScoreEditor() {
  const svg=$('notation-staff'),tabs=$('score-voices');if(!svg||!tabs)return;
  const voices=parseVoices();if(!voices.length)return;
  notationState.voice=Math.min(notationState.voice,voices.length-1);
  tabs.innerHTML=voices.map((voice,index)=>`<button type="button" data-score-voice="${index}" class="${index===notationState.voice?'active':''}" aria-pressed="${index===notationState.voice}"><i style="--voice:${voiceColors[index%voiceColors.length]}"></i>${escapeText(voice.name)}</button>`).join('');
  const timelines=voices.map(parseEvents),beatWidth=64,meterLength=measureBeats(),contentBeats=Math.max(0,...timelines.map(events=>events.reduce((sum,event)=>sum+event.duration,0))),requiredMeasures=Math.max(1,Math.ceil(contentBeats/meterLength));notationState.measures=Math.max(notationState.measures,requiredMeasures);const displayBeats=notationState.measures*meterLength,barStart=196,endBarX=barStart+displayBeats*beatWidth,width=Math.max(1200,endBarX+38),staffGap=166,height=voices.length*staffGap+44;
  const [meterCount,meterValue]=meterParts();$('measure-count').textContent=notationState.measures;$('remove-measure').disabled=notationState.measures<=requiredMeasures;$('remove-voice').disabled=voices.length<=1;$('score-summary').innerHTML=`<strong>${notationState.measures} measure${notationState.measures===1?'':'s'} · ${notationState.timeSignature}</strong><span>${meterPulse()} · click the staff to append an event</span>`;
  svg.setAttribute('viewBox',`0 0 ${width} ${height}`);svg.style.width=`${Math.max(100,width/12)}%`;svg.style.height=`${height}px`;svg.setAttribute('tabindex','0');
  const systems=voices.map((voice,voiceIndex)=>{
    const events=timelines[voiceIndex],sounded=events.filter(event=>!event.rest),averageMidi=sounded.length?sounded.reduce((sum,event)=>sum+event.midi,0)/sounded.length:64,bassClef=averageMidi<60,staffBase=bassClef?pitchIndex('G',2):pitchIndex('E',4),staffTop=42+voiceIndex*staffGap,staffBottom=staffTop+72,selected=voiceIndex===notationState.voice;
    const staff=Array.from({length:5},(_,i)=>`<line x1="108" y1="${staffTop+i*18}" x2="${endBarX}" y2="${staffTop+i*18}"/>`).join('');
    const measureStarts=Array.from({length:notationState.measures},(_,i)=>i*meterLength),bars=measureStarts.map((beat,i)=>{const x=barStart+beat*beatWidth;return `<line class="barline" x1="${x}" y1="${staffTop}" x2="${x}" y2="${staffBottom}"/>${voiceIndex===0?`<text class="measure-number" x="${x+7}" y="${staffTop-13}">${i+1}</text>`:''}`;}).join('');
    const noteX=start=>{const measure=Math.floor(start/meterLength),within=start-measure*meterLength,measureWidth=meterLength*beatWidth;return barStart+measure*measureWidth+30+within*((measureWidth-60)/meterLength);};
    const layouts=events.map(event=>{const x=noteX(event.start+(event.rest?event.duration/2:0)),bare=event.note.replace(/[#b]/,''),match=bare.match(/^([A-G])(\-?\d)$/i),y=event.rest?(staffTop+staffBottom)/2:staffBottom-(pitchIndex(match[1].toUpperCase(),Number(match[2]))-staffBase)*9;return{event,x,y,options:{}};});
    const beams=[];for(let i=0;i<layouts.length-1;i++){const a=layouts[i],b=layouts[i+1];if(a.event.rest||b.event.rest||a.event.duration!==.5||b.event.duration!==.5||Math.abs(a.event.start+.5-b.event.start)>.001||Math.floor(a.event.start)!==Math.floor(b.event.start))continue;const stemUp=(a.y+b.y)/2>=(staffTop+staffBottom)/2,firstEnd=stemUp?a.y-42:a.y+42,slope=Math.max(-9,Math.min(9,b.y-a.y)),secondEnd=firstEnd+slope;a.options={beamed:true,stemUp,stemEnd:firstEnd};b.options={beamed:true,stemUp,stemEnd:secondEnd};const ax=a.x+(stemUp?7:-7),bx=b.x+(stemUp?7:-7),thickness=stemUp?5:-5;beams.push(`<path class="note-beam" d="M${ax} ${firstEnd} L${bx} ${secondEnd} L${bx} ${secondEnd+thickness} L${ax} ${firstEnd+thickness} Z"/>`);i++;}
    const marks=layouts.map(({event,x,y,options})=>`<g class="score-event"><title>${escapeText(voice.name)}, beat ${event.start+1}: ${event.rest?'rest':event.note}, ${durationLabel(event.duration)}</title>${notationEventSvg(event,x,y,staffTop,staffBottom,options)}</g>`).join('');
    return `<g class="score-system${selected?' selected-system':''}" data-system="${voiceIndex}"><rect class="staff-selector" x="0" y="${staffTop-30}" width="${width}" height="${staffGap-5}"/><text class="staff-name" x="52" y="${staffTop+41}" text-anchor="end">${escapeText(voice.name)}</text><g class="staff-lines">${staff}</g><text class="${bassClef?'bass-clef':'treble-clef'}" x="114" y="${bassClef?staffTop+58:staffTop+69}">${bassClef?'𝄢':'𝄞'}</text><text class="time-signature" x="176" y="${staffTop+29}">${meterCount}</text><text class="time-signature" x="176" y="${staffTop+65}">${meterValue}</text><g class="measure-lines">${bars}</g>${marks}${beams.join('')}<line class="end-bar-thin" x1="${endBarX-5}" y1="${staffTop}" x2="${endBarX-5}" y2="${staffBottom}"/><line class="end-bar" x1="${endBarX}" y1="${staffTop}" x2="${endBarX}" y2="${staffBottom}"/></g>`;
  }).join('');
  const firstTop=42,lastBottom=42+(voices.length-1)*staffGap+72;
  const middle=(firstTop+lastBottom)/2,span=lastBottom-firstTop;
  svg.innerHTML=`<rect class="score-hit-area" width="${width}" height="${height}"/><path class="system-brace" d="M100 ${firstTop} C78 ${firstTop+span*.04},90 ${middle-span*.12},72 ${middle} C90 ${middle+span*.12},78 ${lastBottom-span*.04},100 ${lastBottom}"/>${systems}`;
  svg.setAttribute('aria-label',`${voices.length}-voice score with ${voices.map(voice=>voice.name).join(', ')}. Click any staff to append a ${durationLabel(notationState.duration)} ${notationState.kind}.`);
}

function selectNotationTool(selector,key,value) {
  document.querySelectorAll(selector).forEach(button=>{const active=button.dataset[key]===String(value);button.classList.toggle('active',active);button.setAttribute('aria-pressed',String(active));});
}

document.querySelectorAll('[data-kind]').forEach(button=>button.addEventListener('click',()=>{notationState.kind=button.dataset.kind;selectNotationTool('[data-kind]','kind',notationState.kind);$('score-instruction').textContent=notationState.kind==='rest'?'Click any voice staff to place the selected rest value.':'Click a line or space on any staff to add a note to that voice.';renderScoreEditor();}));
document.querySelectorAll('[data-duration]').forEach(button=>button.addEventListener('click',()=>{notationState.duration=Number(button.dataset.duration);selectNotationTool('[data-duration]','duration',button.dataset.duration);renderScoreEditor();}));
document.querySelectorAll('[data-accidental]').forEach(button=>button.addEventListener('click',()=>{notationState.accidental=button.dataset.accidental;selectNotationTool('[data-accidental]','accidental',notationState.accidental);}));
$('time-signature')?.addEventListener('change',event=>{notationState.timeSignature=event.target.value;const content=Math.max(0,...parseVoices().map(voice=>parseEvents(voice).reduce((sum,item)=>sum+item.duration,0)));notationState.measures=Math.max(1,Math.ceil(content/measureBeats()));renderScoreEditor();markResultsStale();});
$('add-measure')?.addEventListener('click',()=>{notationState.measures++;renderScoreEditor();});
$('remove-measure')?.addEventListener('click',()=>{notationState.measures=Math.max(1,notationState.measures-1);renderScoreEditor();});
$('score-voices')?.addEventListener('click',event=>{const button=event.target.closest('[data-score-voice]');if(!button)return;notationState.voice=Number(button.dataset.scoreVoice);voiceSelect.value=notationState.voice;renderScoreEditor();});
$('add-voice')?.addEventListener('click',()=>{const voices=parseVoices(),number=voices.length+1;voices.push({name:`Voice ${number}`,notes:'R/1'});notationState.voice=voices.length-1;setScoreSource(voices);voiceSelect.value=notationState.voice;});
$('remove-voice')?.addEventListener('click',()=>{const voices=parseVoices();if(voices.length<=1)return;voices.splice(notationState.voice,1);notationState.voice=Math.max(0,Math.min(notationState.voice,voices.length-1));setScoreSource(voices);voiceSelect.value=notationState.voice;});
$('notation-staff')?.addEventListener('click',event=>{const svg=$('notation-staff'),point=svg.createSVGPoint();point.x=event.clientX;point.y=event.clientY;const local=point.matrixTransform(svg.getScreenCTM().inverse()),voices=parseVoices(),staffGap=166,voiceIndex=Math.max(0,Math.min(voices.length-1,Math.round((local.y-78)/staffGap))),voice=voices[voiceIndex];if(!voice)return;notationState.voice=voiceIndex;voiceSelect.value=voiceIndex;let token;if(notationState.kind==='rest')token=`R/${notationState.duration}`;else{const events=parseEvents(voice),sounded=events.filter(item=>!item.rest),average=sounded.length?sounded.reduce((sum,item)=>sum+item.midi,0)/sounded.length:64,base=average<60?pitchIndex('G',2):pitchIndex('E',4),staffTop=42+voiceIndex*staffGap,staffBottom=staffTop+72,index=base+Math.round((staffBottom-Math.max(staffTop-36,Math.min(staffBottom+54,local.y)))/9),natural=pitchFromIndex(index),mark=notationState.accidental==='sharp'?'#':notationState.accidental==='flat'?'b':'';token=`${natural[0]}${mark}${natural.slice(1)}/${notationState.duration}`;}voice.notes=`${voice.notes} ${token}`.trim();setScoreSource(voices);});
$('undo-note')?.addEventListener('click',()=>{const voices=parseVoices(),voice=voices[notationState.voice];if(!voice)return;const tokens=voice.notes.split(/[\s,]+/).filter(Boolean);tokens.pop();voice.notes=tokens.join(' ');setScoreSource(voices);});
$('clear-voice')?.addEventListener('click',()=>{const voices=parseVoices(),voice=voices[notationState.voice];if(!voice)return;voice.notes='R/4';setScoreSource(voices);});
$('notation-staff')?.addEventListener('keydown',event=>{if(event.ctrlKey||event.metaKey)return;if(event.key==='Backspace'||event.key==='Delete'){event.preventDefault();$('undo-note').click();return;}if(event.key.toLowerCase()==='m'){event.preventDefault();$('add-measure').click();return;}const durations={1:.5,2:1,3:2,4:4};if(durations[event.key]){event.preventDefault();notationState.duration=durations[event.key];selectNotationTool('[data-duration]','duration',String(durations[event.key]));renderScoreEditor();}});

const activeLayers = new Set(['motion','intervals']);
const voiceColors = ['#d9b86c','#83b8ad','#b49ac4','#d08f74','#7fa5ce','#b7bd78'];
function eventAt(events,time){return events.find(event=>!event.rest&&time>=event.start&&time<event.start+event.duration)||null;}
function analyzeFabric(){
  const voices=parseVoices(),timelines=voices.map(parseEvents),ends=timelines.flat().map(e=>e.start+e.duration),totalBeats=ends.length?Math.max(...ends):0,boundaries=[...new Set([0,totalBeats,...timelines.flatMap(events=>events.flatMap(e=>[e.start,e.start+e.duration]))])].sort((a,b)=>a-b),consonances=new Set([0,3,4,5,7,8,9]),perfect=new Set([0,7]),pairs=[];
  let verticalTotal=0,consonantTotal=0,activeSum=0,samples=0,crossings=0;
  for(let p=0;p<voices.length;p++)for(let q=p+1;q<voices.length;q++){
    const motion={contrary:0,parallel:0,similar:0,oblique:0},intervals={consonant:0,dissonant:0},intervalClasses=Array(12).fill(0),directPerfects=[],links=[];let previous=null,previousOrder=null;
    boundaries.slice(0,-1).forEach((time,index)=>{const a=eventAt(timelines[p],time+.0001),b=eventAt(timelines[q],time+.0001);if(!a||!b){previous=null;previousOrder=null;return;}const ic=Math.abs(a.midi-b.midi)%12,consonant=consonances.has(ic),order=Math.sign(a.midi-b.midi);intervals[consonant?'consonant':'dissonant']++;intervalClasses[ic]++;verticalTotal++;if(consonant)consonantTotal++;if(previousOrder&&order&&order!==previousOrder)crossings++;if(order)previousOrder=order;let kind='';if(previous&&(previous.a!==a.midi||previous.b!==b.midi)){const da=Math.sign(a.midi-previous.a),db=Math.sign(b.midi-previous.b);if(!da||!db)kind='oblique';else if(da!==db)kind='contrary';else if(perfect.has(ic)&&perfect.has(previous.ic)&&ic===previous.ic)kind='parallel';else {kind='similar';if(perfect.has(ic))directPerfects.push({time,ic});}motion[kind]++;}links.push({time,end:boundaries[index+1],a,b,ic,consonant,kind});previous={a:a.midi,b:b.midi,ic};});
    const onsetsA=timelines[p].filter(e=>!e.rest).map(e=>e.start),onsetsB=timelines[q].filter(e=>!e.rest).map(e=>e.start),union=new Set([...onsetsA,...onsetsB]),shared=[...union].filter(t=>onsetsA.includes(t)&&onsetsB.includes(t)).length,independence=union.size?Math.round((1-shared/union.size)*100):0,signature=events=>events.filter(e=>!e.rest).slice(0,4).map((e,i,a)=>i?e.midi-a[i-1].midi:null).slice(1).join(','),imitation=signature(timelines[p])&&signature(timelines[p])===signature(timelines[q]);pairs.push({p,q,motion,intervals,intervalClasses,directPerfects,links,independence,imitation});
  }
  for(let time=0;time<totalBeats;time+=.5){activeSum+=timelines.filter(events=>eventAt(events,time+.001)).length;samples++;}
  const entrances=timelines.flatMap((events,index)=>{const first=events.find(e=>!e.rest);return first?[{time:first.start,voice:index}]:[];}),cadenceTimes=boundaries.slice(1,-1).filter(time=>{const before=timelines.map(e=>eventAt(e,time-.001)),after=timelines.map(e=>eventAt(e,time+.001));return before.filter((e,i)=>e&&after[i]&&e.midi!==after[i].midi).length>=Math.max(2,Math.ceil(voices.length*.66));});
  return{voices,timelines,totalBeats,pairs,entrances,cadenceTimes,verticalTotal,consonantTotal,crossings,density:samples?activeSum/samples:0};
}
function midiName(midi){const names=['C','C♯','D','E♭','E','F','F♯','G','A♭','A','B♭','B'];return names[(midi%12+12)%12]+(Math.floor(midi/12)-1);}
function intervalName(ic){return['unison/octave','minor 2nd','major 2nd','minor 3rd','major 3rd','perfect 4th','tritone','perfect 5th','minor 6th','major 6th','minor 7th','major 7th'][ic]||'interval';}
function updateFocusedReading(fabric,selected){
  const voice=fabric.voices[selected]; if(!voice)return;
  const events=fabric.timelines[selected].filter(event=>!event.rest),related=fabric.pairs.filter(pair=>pair.p===selected||pair.q===selected),totals={contrary:0,parallel:0,similar:0,oblique:0};
  related.forEach(pair=>Object.keys(totals).forEach(key=>totals[key]+=pair.motion[key]));
  const stable=related.reduce((sum,pair)=>sum+pair.intervals.consonant,0),vertical=related.reduce((sum,pair)=>sum+pair.intervals.consonant+pair.intervals.dissonant,0),range=events.length?Math.max(...events.map(event=>event.midi))-Math.min(...events.map(event=>event.midi)):0,entrance=events[0]?.start||0,dominant=Object.entries(totals).sort((a,b)=>b[1]-a[1])[0]?.[0]||'independent';
  const context=$('focus-context');context.style.setProperty('--focus-color',voiceColors[selected%voiceColors.length]);context.innerHTML=`<span><strong>Following ${escapeText(voice.name)}</strong><br>${events.length} sounding event${events.length===1?'':'s'} · ${range}-semitone range · enters at beat ${entrance+1}</span><b>${related.length} live connection${related.length===1?'':'s'}</b>`;
  $('m-voices').textContent=`${voice.name} / ${related.length}`;$('m-motion').textContent=`${totals.contrary} · ${totals.parallel} · ${totals.similar} · ${totals.oblique}`;$('m-stability').textContent=`${vertical?Math.round(stable/vertical*100):0}%`;$('m-density').textContent=fabric.density.toFixed(1);$('pairwise-title').textContent=`${voice.name} with every other line`;
  document.querySelectorAll('.pair-row').forEach(row=>row.classList.toggle('is-secondary',Number(row.dataset.p)!==selected&&Number(row.dataset.q)!==selected));
  $('insight-title').textContent=`${voice.name}: ${dominant} motion leads the exchange`;$('insight-copy').textContent=`Follow ${voice.name} while the supporting voices remain visible. Its ${related.length} relationship${related.length===1?'':'s'} contain ${totals.contrary} contrary, ${totals.parallel} perfect-parallel, ${totals.similar} similar, and ${totals.oblique} oblique changes; ${vertical?Math.round(stable/vertical*100):0}% of its sampled vertical spans fall within the lab’s provisional consonance classes. Switch focus to hear the same passage reorganize around another line.`;
}
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
  svg.setAttribute('aria-label',`${voices[selected]?.name||'Selected voice'} is emphasized while ${Math.max(voices.length-1,0)} supporting voices remain visible across ${totalBeats} beats`);
  $('fabric-label').textContent=`Following ${voices[selected]?.name||'voice'} through the complete texture · ${totalBeats} beats`;
  $('voice-legend').innerHTML=voices.map((voice,index)=>`<button type="button" class="${index===selected?'traced':''}" data-voice="${index}" aria-pressed="${index===selected}"><i style="--voice:${voiceColors[index%voiceColors.length]}"></i>${escapeText(voice.name)}<small>${index===selected?'current listening focus':'follow this voice'}</small></button>`).join('');
  $('voice-legend').querySelectorAll('button').forEach(button=>button.addEventListener('click',()=>{stopAudio();voiceSelect.value=button.dataset.voice;renderTexture();}));
  updateFocusedReading(fabric,selected);
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

function renderAnalysis(data){const fabric=analyzeFabric(),totals={contrary:0,parallel:0,similar:0,oblique:0};fabric.pairs.forEach(pair=>Object.keys(totals).forEach(key=>totals[key]+=pair.motion[key]));const pairCount=fabric.voices.length*(fabric.voices.length-1)/2,stability=fabric.verticalTotal?Math.round(fabric.consonantTotal/fabric.verticalTotal*100):0;renderTexture();$('pairwise-list').innerHTML=fabric.pairs.map(pair=>{const m=pair.motion,stable=pair.intervals.consonant+pair.intervals.dissonant?Math.round(pair.intervals.consonant/(pair.intervals.consonant+pair.intervals.dissonant)*100):0,prominent=pair.intervalClasses.map((count,ic)=>({count,ic})).filter(item=>item.count).sort((a,b)=>b.count-a.count).slice(0,2).map(item=>intervalName(item.ic)).join(' · ');return`<div class="pair-row" data-p="${pair.p}" data-q="${pair.q}"><div><i style="--a:${voiceColors[pair.p%voiceColors.length]};--b:${voiceColors[pair.q%voiceColors.length]}"></i><strong>${escapeText(fabric.voices[pair.p].name)} ↔ ${escapeText(fabric.voices[pair.q].name)}</strong><small>${stable}% provisional consonance · ${pair.independence}% rhythmic independence${pair.imitation?' · opening contour correspondence':''}</small></div><p class="interval-profile">Frequent spans: ${escapeText(prominent||'—')}${pair.directPerfects.length?` · ${pair.directPerfects.length} direct perfect arrival${pair.directPerfects.length===1?'':'s'} for review`:''}</p><dl><span><b>${m.contrary}</b> contrary</span><span><b>${m.parallel}</b> perfect parallel</span><span><b>${m.similar}</b> similar</span><span><b>${m.oblique}</b> oblique</span></dl></div>`;}).join('')||'<p class="empty">Add a second voice to begin a relational reading.</p>';const resolutions=fabric.pairs.flatMap(pair=>pair.links.map((link,i)=>!link.consonant&&pair.links[i+1]?.consonant?{time:pair.links[i+1].time,label:'Dissonance-to-consonance motion',copy:`${fabric.voices[pair.p].name} and ${fabric.voices[pair.q].name} move into ${intervalName(pair.links[i+1].ic)}; audition its preparation and function.`}:null).filter(Boolean)).slice(0,4),directs=fabric.pairs.flatMap(pair=>pair.directPerfects.map(item=>({time:item.time,label:`Direct ${intervalName(item.ic)}`,copy:`${fabric.voices[pair.p].name} and ${fabric.voices[pair.q].name} approach in similar motion; review in stylistic context.`}))).slice(0,3),events=[...fabric.entrances.map(e=>({time:e.time,label:`${fabric.voices[e.voice].name} enters`,copy:e.time?'Staggered entrance expands the active texture.':'Present at the opening of the texture.'})),...resolutions,...directs,...fabric.cadenceTimes.slice(-3).map(t=>({time:t,label:'Coordinated change',copy:'Multiple voices redirect together: test this point for convergence or structural cadence.'}))].sort((a,b)=>a.time-b.time);$('event-list').innerHTML=events.map(e=>`<div class="event-row"><b>${e.time.toFixed(1)}</b><p><strong>${escapeText(e.label)}</strong><span>${escapeText(e.copy)}</span></p></div>`).join('')||'<p class="empty">No shared structural events detected.</p>';latestReading={created:new Date().toISOString(),score:notes.value,voices:fabric.voices.map(voice=>voice.name),beats:fabric.totalBeats,pairs:pairCount,motion:totals,provisionalConsonancePercent:stability,meanActiveVoices:Number(fabric.density.toFixed(2)),registralCrossings:fabric.crossings,methodologicalNote:'Pitch, onset, duration, interval, and motion are measured. Stability, imitation, convergence, and resolution are heuristic prompts requiring musical context.'};$('saved-badge').textContent=data.id?'✓ Saved to research archive':'Analyzed locally';results.classList.remove('hidden','is-stale');updateFocusedReading(fabric,Number(voiceSelect.value||0));results.scrollIntoView({behavior:'smooth',block:'start'});}

form.addEventListener('submit',async event=>{event.preventDefault();error.textContent='';const button=form.querySelector('.analyze'),old=button.innerHTML;button.disabled=true;button.textContent='Reading the fabric…';try{const voices=syncVoices();validateScore(voices);const focus=voices[Number(voiceSelect.value||0)];if(!focus)throw new Error('Enter at least one voice using “Name: C4 D4 E4”.');const focusNotes=parseEvents(focus,true).filter(e=>!e.rest).map(e=>e.note).join(' ');if(!focusNotes)throw new Error('The traced voice needs at least one sounding note.');const response=await fetch('/api/analyze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({notes:focusNotes,score:notes.value})});const data=await response.json();if(!response.ok)throw new Error(data.error||'Analysis failed');renderAnalysis(data);await loadHistory();}catch(cause){error.textContent=cause.message;}finally{button.disabled=false;button.innerHTML=old;}});

function escapeText(text){const span=document.createElement('span');span.textContent=text;return span.innerHTML;}
function exportReading(){
  if(!latestReading){error.textContent='Analyze a score before exporting a research note.';return;}
  const lines=['FISOBACH · CONTRAPUNTAL RESEARCH NOTE',`Generated ${latestReading.created}`,`Score plan: ${notationState.timeSignature} · ${notationState.measures} measure${notationState.measures===1?'':'s'}`,'',latestReading.methodologicalNote,'',`Voices: ${latestReading.voices.join(' · ')}`,`Span: ${latestReading.beats} beats · ${latestReading.pairs} voice pairs`,`Motion: ${latestReading.motion.contrary} contrary · ${latestReading.motion.parallel} perfect parallel · ${latestReading.motion.similar} similar · ${latestReading.motion.oblique} oblique`,`Provisional consonance: ${latestReading.provisionalConsonancePercent}%`,`Mean active voices: ${latestReading.meanActiveVoices}`,`Registral crossings: ${latestReading.registralCrossings}`,'','ENTERED SCORE',latestReading.score,'','Interpretive reminder: verify every proposed relation by ear, in the source, and within stylistic context.'];
  const url=URL.createObjectURL(new Blob([lines.join('\n')],{type:'text/plain;charset=utf-8'})),link=document.createElement('a');link.href=url;link.download=`fisobach-reading-${new Date().toISOString().slice(0,10)}.txt`;link.click();setTimeout(()=>URL.revokeObjectURL(url),0);
}
async function loadHistory(){const history=$('history');try{const response=await fetch('/api/analyses',{cache:'no-store'}),data=await response.json();if(!response.ok)throw new Error();if(!data.analyses.length){history.innerHTML='<p class="empty">Your analyzed subjects will appear here.</p>';return;}history.innerHTML=data.analyses.map(item=>{const score=item.score||`Voice I: ${item.notes}`,voiceNames=score.split(/\n+/).map(line=>line.match(/^([^:]+):/)?.[1].trim()).filter(Boolean),title=voiceNames.length>1?voiceNames.join(' · '):item.notes;return`<button class="history-item" type="button"><span class="history-id">${String(item.id).padStart(3,'0')}</span><span><strong>${escapeText(title)}</strong><small>${item.created_at} UTC · ${voiceNames.length||1} voice${voiceNames.length===1?'':'s'} · ${item.summary.note_count} focus notes · ${item.summary.range_semitones} semitones</small></span><b>Revisit score →</b></button>`;}).join('');history.querySelectorAll('.history-item').forEach((element,index)=>element.addEventListener('click',()=>{const item=data.analyses[index];notes.value=item.score||`Voice I: ${item.notes}`;notationState.voice=0;syncVoices();if(!results.classList.contains('hidden')){results.classList.add('is-stale');$('saved-badge').textContent='Archive score loaded · analyze again';}$('workbench').scrollIntoView({behavior:'smooth',block:'start'});$('notation-staff')?.focus({preventScroll:true});}));}catch{history.innerHTML='<p class="empty">Archive unavailable. Your server may still be waking up.</p>';}}

function markResultsStale(){if(results.classList.contains('hidden'))return;results.classList.add('is-stale');$('saved-badge').textContent='Score changed · analyze again';}
$('refresh').addEventListener('click',loadHistory);$('export-analysis').addEventListener('click',exportReading);notes.addEventListener('input',()=>{syncVoices();markResultsStale();});notes.addEventListener('keydown',event=>{if((event.ctrlKey||event.metaKey)&&event.key==='Enter')form.requestSubmit();});voiceSelect.addEventListener('change',()=>{stopAudio();notationState.voice=Number(voiceSelect.value||0);renderScoreEditor();if(!results.classList.contains('hidden')&&!results.classList.contains('is-stale'))renderTexture();});tempo.addEventListener('input',()=>{$('tempo-value').textContent=`${tempo.value} BPM`;});$('play-texture').addEventListener('click',()=>playVoices(false));$('play-focus').addEventListener('click',()=>playVoices(true));$('stop-audio').addEventListener('click',stopAudio);document.querySelectorAll('[data-layer]').forEach(button=>button.addEventListener('click',()=>{const layer=button.dataset.layer;if(activeLayers.has(layer))activeLayers.delete(layer);else activeLayers.add(layer);button.classList.toggle('active',activeLayers.has(layer));button.setAttribute('aria-pressed',String(activeLayers.has(layer)));if(!results.classList.contains('hidden')&&!results.classList.contains('is-stale'))renderTexture();}));if(/^Archived voice\s*:/i.test(notes.value))notes.value=notes.value.replace(/^Archived voice\s*:/i,'Voice I:');syncVoices();loadHistory();

if('IntersectionObserver'in window){const navLinks=[...document.querySelectorAll('nav a[href^="#"]')],navObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(!entry.isIntersecting)return;navLinks.forEach(link=>link.toggleAttribute('aria-current',link.getAttribute('href')===`#${entry.target.id}`));}),{rootMargin:'-25% 0px -65%'});navLinks.forEach(link=>{const section=document.querySelector(link.getAttribute('href'));if(section)navObserver.observe(section);});const revealObserver=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){entry.target.classList.add('is-visible');revealObserver.unobserve(entry.target);}}),{rootMargin:'0px 0px -8%',threshold:.08});document.querySelectorAll('.method-intro,.method-story,.method-image,.practice-steps,.method-result,.invitation>*,.studio-top,#analysis-form,.section-heading,.metric-grid,.analysis-grid,.insight,.history').forEach(element=>{element.classList.add('reveal');revealObserver.observe(element);});}
const progressBar=document.querySelector('.reading-progress i');const updateProgress=()=>{const distance=document.documentElement.scrollHeight-innerHeight;progressBar.style.transform=`scaleX(${distance>0?Math.min(scrollY/distance,1):0})`;};addEventListener('scroll',updateProgress,{passive:true});addEventListener('resize',updateProgress);updateProgress();
