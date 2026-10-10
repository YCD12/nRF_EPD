(function () {
  'use strict';

  function loadStatistics() {
    if (document.querySelector('script[src*="busuanzi/2.3/busuanzi.pure.mini.js"]')) return;
    const script = document.createElement('script');
    script.async = true;
    script.src = 'https://busuanzi.ibruce.info/busuanzi/2.3/busuanzi.pure.mini.js';
    document.body.appendChild(script);
  }

  function scheduleStatistics() {
    if (typeof window.requestIdleCallback === 'function') {
      window.requestIdleCallback(loadStatistics, { timeout: 2000 });
    } else {
      window.setTimeout(loadStatistics, 200);
    }
  }

  // Optional statistics start once the page is ready and the browser is idle.
  if (document.readyState === 'complete') {
    scheduleStatistics();
  } else {
    window.addEventListener('load', scheduleStatistics, { once: true });
  }
})();
