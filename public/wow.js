(() => {
  const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const revealTargets = document.querySelectorAll(
    '.method-intro,.method-story,.method-image,.practice-steps,.method-result,.invitation>*,' +
    '.studio-top,#analysis-form,.section-heading,.metric-grid,.analysis-grid,.insight,.history'
  );

  revealTargets.forEach(element => element.dataset.reveal = '');

  if (!reduceMotion && 'IntersectionObserver' in window) {
    document.documentElement.classList.add('reveal-ready');
    const observer = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        if (!entry.isIntersecting) return;
        entry.target.classList.add('is-visible');
        observer.unobserve(entry.target);
      });
    }, { threshold: 0.12, rootMargin: '0px 0px -6% 0px' });
    revealTargets.forEach(element => observer.observe(element));
  } else {
    revealTargets.forEach(element => element.classList.add('is-visible'));
  }

  if (!reduceMotion && matchMedia('(pointer:fine)').matches) {
    const glow = document.createElement('div');
    glow.className = 'cursor-glow';
    glow.setAttribute('aria-hidden', 'true');
    document.body.appendChild(glow);
    let x = innerWidth / 2, y = innerHeight / 2, targetX = x, targetY = y;
    addEventListener('pointermove', event => { targetX = event.clientX; targetY = event.clientY; });
    const animate = () => {
      x += (targetX - x) * 0.11;
      y += (targetY - y) * 0.11;
      glow.style.transform = `translate(${x - 180}px,${y - 180}px)`;
      requestAnimationFrame(animate);
    };
    animate();
  }

  const hero = document.querySelector('.hero');
  const heroImage = document.querySelector('.hero-image img');
  if (!reduceMotion && hero && heroImage) {
    hero.addEventListener('pointermove', event => {
      const box = hero.getBoundingClientRect();
      const dx = (event.clientX - box.left) / box.width - 0.5;
      const dy = (event.clientY - box.top) / box.height - 0.5;
      heroImage.style.objectPosition = `${50 + dx * 2.2}% ${50 + dy * 1.4}%`;
    });
  }
})();
