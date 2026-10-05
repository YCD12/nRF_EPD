(function () {
  'use strict';

  function loadStatistics() {
    const script = document.createElement('script');
    script.async = true;
    script.src = 'https://busuanzi.ibruce.info/busuanzi/2.3/busuanzi.pure.mini.js';
    document.body.appendChild(script);
  }

  // Initialize the console before requesting the external counter service.
  if (document.readyState === 'complete') {
    loadStatistics();
  } else {
    window.addEventListener('load', loadStatistics, { once: true });
  }
})();
