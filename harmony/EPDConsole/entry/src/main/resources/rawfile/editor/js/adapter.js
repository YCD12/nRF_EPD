const canvas = document.getElementById('canvas');
const ctx = canvas.getContext('2d');

// stubs required by paint.js
function setCanvasTitle() {}
function fillCanvas(style) {
  ctx.fillStyle = style;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
}

let bgImage = null;
let bgOpacity = 1;

function drawBackground() {
  if (bgImage === null) {
    return;
  }
  ctx.save();
  ctx.globalAlpha = bgOpacity;
  ctx.drawImage(bgImage, 0, 0, canvas.width, canvas.height);
  ctx.restore();
}

const origFillRect = ctx.fillRect.bind(ctx);
ctx.fillRect = function (x, y, w, h) {
  origFillRect(x, y, w, h);
  if (x === 0 && y === 0 && w >= canvas.width && h >= canvas.height) {
    drawBackground();
  }
};

const opacityInput = document.getElementById('bg-opacity');
const opacityLabel = document.getElementById('bg-opacity-value');
if (opacityInput !== null) {
  opacityInput.addEventListener('input', function () {
    bgOpacity = parseInt(opacityInput.value, 10) / 100;
    if (opacityLabel !== null) {
      opacityLabel.textContent = opacityInput.value + '%';
    }
    if (paintManager && paintManager.redrawAll) {
      paintManager.redrawAll();
    }
  });
}

const paintManager = new PaintManager(canvas, ctx);
paintManager.initPaintTools();

function loadImage(dataUrl) {
  const img = new Image();
  img.onload = function () {
    bgImage = img;
    canvas.width = img.width;
    canvas.height = img.height;
    ctx.fillStyle = '#ffffff';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    drawBackground();
    paintManager.clearElements();
    paintManager.historyStack = [];
    paintManager.historyStep = -1;
    paintManager.saveToHistory();
    paintManager.setActiveTool(null, '');
  };
  img.src = dataUrl;
}

function exportImage() {
  const data = canvas.toDataURL('image/png');
  if (window.appProxy && window.appProxy.onEdited) {
    window.appProxy.onEdited(data);
  }
}

document.getElementById('done-btn').addEventListener('click', exportImage);
document.getElementById('cancel-btn').addEventListener('click', function () {
  if (window.appProxy && window.appProxy.onCancel) {
    window.appProxy.onCancel();
  }
});

window.loadImage = loadImage;