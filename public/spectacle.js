(() => {
  const rain = document.querySelector('.score-rain');
  if (rain && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    const symbols = ['♪','♫','♩','𝅗𝅥','♭','♯','𝄞'];
    for (let index = 0; index < 24; index++) {
      const note = document.createElement('i');
      note.textContent = symbols[index % symbols.length];
      note.style.setProperty('--x', `${4 + Math.random() * 92}%`);
      note.style.setProperty('--d', `${8 + Math.random() * 10}s`);
      note.style.setProperty('--delay', `${-Math.random() * 15}s`);
      note.style.fontSize = `${1 + Math.random() * 2.2}rem`;
      rain.appendChild(note);
    }
  }

  document.querySelectorAll('.voice-card').forEach((card, index) => {
    card.addEventListener('pointermove', event => {
      if (!matchMedia('(pointer:fine)').matches) return;
      const box = card.getBoundingClientRect();
      const x = (event.clientX - box.left) / box.width - .5;
      const y = (event.clientY - box.top) / box.height - .5;
      card.style.transform = `translateY(-18px) rotateX(${-y * 5}deg) rotateY(${x * 7}deg)`;
    });
    card.addEventListener('pointerleave', () => { card.style.transform = ''; });
    card.style.transitionDelay = `${index * 60}ms`;
  });
})();
