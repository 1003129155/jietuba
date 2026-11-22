// Runs inside the page. Returns the visible text lines and the DOM block each one belongs to.
// A "block" is the nearest ancestor element whose computed display is not inline: that is what
// a reader sees as one paragraph / list item / table cell / button. Its lines are the visual
// lines of its text, built from per-character client rects.
() => {
  const VW = window.innerWidth, VH = window.innerHeight;
  const SKIP = 'script,style,noscript,textarea,select,option,svg,canvas,head,template,title,iframe,object';
  const blockIds = new Map();
  const blocks = [];
  const blockOf = (el) => {
    while (el && el !== document.body && el !== document.documentElement) {
      const d = getComputedStyle(el).display;
      const inlineLike = d === 'inline' || d === 'contents' || d.startsWith('ruby') || /^inline-(block|flex|grid|table)$/.test(d);
      if (!inlineLike || el.tagName === 'BUTTON') return el;
      el = el.parentElement;
    }
    return document.body;
  };
  const opacityOf = (el) => {
    let o = 1;
    for (; el && el !== document.documentElement; el = el.parentElement) o *= parseFloat(getComputedStyle(el).opacity || '1');
    return o;
  };
  const registerBlock = (el) => {
    if (blockIds.has(el)) return blockIds.get(el);
    const cs = getComputedStyle(el);
    const id = blocks.length;
    let ignore = '';
    if (el.closest('pre')) ignore = 'pre';
    else if (/^pre/.test(cs.whiteSpace) || cs.whiteSpace === 'break-spaces') {
      if ((el.textContent || '').indexOf('\n') >= 0) ignore = 'preformatted newline';
    }
    if (!ignore) {
      for (const br of el.querySelectorAll('br')) {
        if (blockOf(br.parentElement) === el) { ignore = 'br'; break; }
      }
    }
    if (!ignore && /^(INPUT|BUTTON)$/.test(el.tagName) === false && el.isContentEditable) ignore = 'editable';
    blocks.push({ id, tag: el.tagName.toLowerCase(), cls: (el.className && el.className.baseVal === undefined ? String(el.className) : '').slice(0, 60), ignore, align: cs.textAlign, fs: parseFloat(cs.fontSize), color: cs.color });
    blockIds.set(el, id);
    return id;
  };

  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  const range = document.createRange();
  const chars = []; // {c, x0,y0,x1,y1, b, el}
  let node;
  while ((node = walker.nextNode())) {
    const text = node.nodeValue;
    if (!text || !text.trim()) continue;
    const parent = node.parentElement;
    if (!parent || parent.closest(SKIP)) continue;
    const cs = getComputedStyle(parent);
    if (cs.visibility !== 'visible' || cs.display === 'none') continue;
    if (opacityOf(parent) < 0.35) continue;
    range.selectNodeContents(node);
    const nr = range.getBoundingClientRect();
    if (nr.width === 0 || nr.height === 0) continue;
    if (nr.bottom < 0 || nr.top > VH || nr.right < 0 || nr.left > VW) continue;
    const blk = blockOf(parent);
    const bid = registerBlock(blk);
    for (let i = 0; i < text.length; ) {
      const cp = text.codePointAt(i), len = cp > 0xffff ? 2 : 1, ch = text.substr(i, len);
      range.setStart(node, i); range.setEnd(node, i + len);
      const rs = range.getClientRects();
      const r = rs.length ? rs[0] : null;
      if (/\s/.test(ch)) chars.push({ c: ' ', b: bid, space: true });
      else if (r && r.width > 0 && r.height > 0) chars.push({ c: ch, x0: r.left, y0: r.top, x1: r.right, y1: r.bottom, b: bid, el: parent, blk });
      i += len;
    }
  }

  // Visual lines per block, in DOM order.
  const lines = [];
  const perBlock = new Map();
  for (const ch of chars) { if (!perBlock.has(ch.b)) perBlock.set(ch.b, []); perBlock.get(ch.b).push(ch); }
  for (const [bid, list] of perBlock) {
    let cur = null;
    const flush = () => {
      if (cur && cur.text.trim()) lines.push(cur);
      cur = null;
    };
    for (const ch of list) {
      if (ch.space) { if (cur && !cur.text.endsWith(' ')) cur.text += ' '; continue; }
      const cy = (ch.y0 + ch.y1) / 2;
      if (cur && !(cy >= cur.y0 && cy <= cur.y1)) flush();
      if (cur && ch.x1 < cur.x0 - 4 * (ch.y1 - ch.y0)) flush();
      if (cur && ch.x0 - cur.x1 > 3 * (ch.y1 - ch.y0)) flush();
      if (!cur) cur = { b: bid, text: '', x0: ch.x0, y0: ch.y0, x1: ch.x1, y1: ch.y1, blk: ch.blk };
      cur.text += ch.c;
      cur.x0 = Math.min(cur.x0, ch.x0); cur.y0 = Math.min(cur.y0, ch.y0);
      cur.x1 = Math.max(cur.x1, ch.x1); cur.y1 = Math.max(cur.y1, ch.y1);
    }
    flush();
  }

  // Keep lines that are on screen and not covered by something else.
  const out = [];
  for (const l of lines) {
    const w = l.x1 - l.x0, h = l.y1 - l.y0;
    if (w < 3 || h < 5 || h > 160) continue;
    const ix0 = Math.max(0, l.x0), iy0 = Math.max(0, l.y0), ix1 = Math.min(VW, l.x1), iy1 = Math.min(VH, l.y1);
    if ((ix1 - ix0) * (iy1 - iy0) < 0.9 * w * h) continue;
    const cy = (l.y0 + l.y1) / 2;
    let seen = 0;
    for (const fx of [0.05, 0.5, 0.95]) {
      const hit = document.elementFromPoint(Math.min(VW - 1, Math.max(0, l.x0 + w * fx)), Math.min(VH - 1, Math.max(0, cy)));
      if (!hit) continue;
      if (l.blk.contains(hit) || hit.contains(l.blk)) seen++;
    }
    if (seen < 2) continue;
    const text = l.text.replace(/\s+/g, ' ').trim();
    if (!text) continue;
    out.push({ b: l.b, text, box: [l.x0, l.y0, l.x1, l.y1] });
  }
  return { w: VW, h: VH, blocks, lines: out };
}
