'use strict';
(async()=>{
  const match=location.hash.match(/^#muse-usage-check=([0-9a-f]{32})$/);
  if(!match)return;
  const id=match[1];
  const wait=async fn=>{const deadline=Date.now()+25000;while(Date.now()<deadline){let value=fn();if(value)return value;await new Promise(r=>setTimeout(r,300));}throw new Error('page');};
  const visible=el=>el && el.getBoundingClientRect().width>0 && el.getBoundingClientRect().height>0;
  let stage='menu-button';
  try{
    let menuAttempt=0;
    let menu=await wait(()=>[...document.querySelectorAll('button[aria-label="Settings"]')].find(visible));
    // The server-rendered button can precede React hydration. Retry only while
    // closed, then wait for the actual menu rather than assuming dispatch worked.
    stage='menu-open';
    await wait(()=>{
      menu=[...document.querySelectorAll('button[aria-label="Settings"]')].find(visible);
      if(!menu)return false;
      if(menu.getAttribute('aria-expanded')==='true')return true;
      if(Date.now()-(menuAttempt||0)<1000)return false;
      menuAttempt=Date.now();menu.focus();
      menu.dispatchEvent(new PointerEvent('pointerdown',{bubbles:true,cancelable:true,composed:true,button:0,buttons:1,pointerType:'mouse',pointerId:1,isPrimary:true,view:window}));
      menu.dispatchEvent(new PointerEvent('pointerup',{bubbles:true,cancelable:true,composed:true,button:0,buttons:0,pointerType:'mouse',pointerId:1,isPrimary:true,view:window}));
      return menu.getAttribute('aria-expanded')==='true';
    });
    stage='settings-item';
    const item=await wait(()=>[...document.querySelectorAll('[role="menuitem"]')].find(el=>visible(el)&&/^Settings(?:Ctrl|\s|$)/.test(el.textContent.trim())));item.click();
    stage='usage-card';
    const value=await wait(()=>{
      for(const bar of document.querySelectorAll('[role="progressbar"]')){
        if(!visible(bar))continue;
        const card=bar.parentElement?.parentElement;
        if(!card)continue;
        const text=card.innerText;
        if(text.length>400)continue;
        const used=text.match(/\b([0-9]{1,3})%\s+used\b/i),reset=text.match(/Weekly limit resets on ([A-Za-z]{3,9} [0-9]{1,2})\b/);
        const raw=bar.getAttribute('aria-valuenow');
        if(used&&reset&&raw!==null&&Number(raw)===Number(used[1])&&Number(raw)>=0&&Number(raw)<=100)return{used:Number(raw),reset:reset[1]};
      }return null;
    });
    chrome.runtime.sendMessage({type:'observation',id,...value});
  }catch{
    const login=!!document.querySelector('input[type=\"password\"],input[type=\"email\"]') || /\/(?:login|signin|auth)(?:\/|$)/i.test(location.pathname);
    chrome.runtime.sendMessage({type:'error',id,reason:login?'login':'page',stage});
  }
})();
