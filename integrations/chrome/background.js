'use strict';
let port, reconnect, jobs=new Map();
function connect(){
  try{
    port=chrome.runtime.connectNative('com.dread7us.muse_storyteller');
    port.onDisconnect.addListener(()=>{void chrome.runtime.lastError;port=null;clearTimeout(reconnect);reconnect=setTimeout(connect,15000);});
    port.onMessage.addListener(async message=>{
      if(message?.type!=='check'||!/^\w{32}$/.test(message.id)||!/^[0-9a-f]{32}$/.test(message.id))return;
      if([...jobs.values()].includes(message.id))return;
      try{let tab=await chrome.tabs.create({url:'https://muse.ai/#muse-usage-check='+message.id,active:true});jobs.set(tab.id,message.id);}
      catch{port?.postMessage({type:'error',id:message.id,reason:'page'});}
    });
  }catch{clearTimeout(reconnect);reconnect=setTimeout(connect,15000);}
}
chrome.runtime.onMessage.addListener((message,sender)=>{
  let id=jobs.get(sender.tab?.id);
  if(!id || sender.id!==chrome.runtime.id || sender.frameId!==0 || sender.origin!=='https://muse.ai' || message?.id!==id)return;
  if(message.type==='observation' && Number.isInteger(message.used) && message.used>=0 && message.used<=100 && /^[A-Za-z]{3,9} [0-9]{1,2}$/.test(message.reset)){
    port?.postMessage({type:'observation',id,used:message.used,reset:message.reset});
    jobs.delete(sender.tab.id);chrome.tabs.remove(sender.tab.id).catch(()=>{});
  }else if(message.type==='error' && ['page','login'].includes(message.reason)){
    port?.postMessage({type:'error',id,reason:message.reason,stage:message.stage});jobs.delete(sender.tab.id);
    if(message.reason!=='login')chrome.tabs.remove(sender.tab.id).catch(()=>{});
  }
});
chrome.tabs.onRemoved.addListener(tabId=>{let id=jobs.get(tabId);if(id){jobs.delete(tabId);port?.postMessage({type:'error',id,reason:'page'});}});
connect();
