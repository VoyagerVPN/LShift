// Проверки работающего приложения: настоящий SendInput -> Raw Input -> worker -> поле.
// CDP используется только для подготовки собственного редактора и чтения результата.
export async function runLiveCases({evaluate, probe, env, browserPid, pause, repeat = 1, filter = null}) {
  const cases = ['input', 'textarea', 'contenteditable'].flatMap(kind => [
    {kind, name:'plain', tokens:[...'ghbdtn'], expected:'привет', roundTrip:true},
    {kind, name:'backspace-empty', tokens:['<Backspace>',...'ghbdtn'], expected:'привет'},
    {kind, name:'erase-overrun', tokens:[...'old',...Array(4).fill('<Backspace>'),...'ghbdtn'], expected:'привет'},
    {kind, name:'top-digit', tokens:[...'ghbdtn1'], expected:'привет1'},
    {kind, name:'numpad-digit', tokens:[...'ghbdtn','<Num1>'], before:'ghbdtn1', expected:'привет1'},
    {kind, name:'numpad-prefix', tokens:['<Num1>',...'ghbdtn'], before:'1ghbdtn', expected:'1привет'},
    {kind, name:'digit-space', tokens:[...'1 ghbdtn'], expected:'1 привет'},
    {kind, name:'history-capacity', tokens:[...('word '.repeat(25)+'ghbdtn')], expected:'word '.repeat(25)+'привет'},
    {kind, name:'history-capacity-space', tokens:[...('word '.repeat(25)+'ghbdtn ')], expected:'word '.repeat(25)+'привет '},
    {kind, name:'select-all-delete', tokens:[...'old','<Ctrl+A>','<Backspace>',...'ghbdtn'], expected:'привет'},
    {kind, name:'select-all-replace', tokens:[...'old','<Ctrl+A>',...'ghbdtn'], expected:'привет'},
    {kind, name:'select-all-extra-delete', tokens:[...'old','<Ctrl+A>',...Array(3).fill('<Backspace>'),...'ghbdtn'], expected:'привет'},
    {kind, name:'escape-after-word', tokens:[...'ghbdtn','<Escape>'], expected:'привет'},
    {kind, name:'control-tap-after-word', tokens:[...'ghbdtn','<Ctrl>'], expected:'привет'},
    {kind, name:'single-shift-before-gesture', tokens:[...'ghbdtn','<Shift>'], settle:400, expected:'привет'},
    {kind, name:'tab-away-back-retype', tokens:[...'old','<Tab>','<Shift+Tab>','<Ctrl+A>','<Backspace>',...'ghbdtn'], expected:'привет'},
    {kind, name:'mouse-away-back-retype', tokens:[...'old'], focusCycle:'mouse', then:['<Ctrl+A>','<Backspace>',...'ghbdtn'], expected:'привет'},
    {kind, name:'alt-tab-back-retype', tokens:[...'old'], focusCycle:'alt-tab', then:['<Ctrl+A>','<Backspace>',...'ghbdtn'], expected:'привет'},
    {kind, name:'user-selection', tokens:[...'ghbdtn','<Ctrl+A>'], selected:true, before:'ghbdtn', expected:'привет'},
    {kind, name:'uncertain-partial-guard', seed:'old', tokens:['<Backspace>',...'ghbdtn'], before:'olghbdtn', expected:'olghbdtn', unchanged:true},
    {kind, name:'fast-focus-type', tokens:[...'ghbdtn'], focusDelay:0, expected:'привет'},
    {kind, name:'focus-during-command', tokens:[...'ghbdtn'], cancelWithTab:true},
  ]);
  const simple = {'<Backspace>':8,'<Delete>':46,'<Escape>':27,'<Tab>':9,'<Left>':37,'<Right>':39};
  const stroke = vk => [[vk,1],[vk,0]];
  const events = tokens => tokens.flatMap(token => {
    if (token in simple) return stroke(simple[token]);
    if (token === '<Ctrl+A>') return [[162,1],...stroke(65),[162,0]];
    if (token === '<Shift+Tab>') return [[160,1],...stroke(9),[160,0]];
    if (token === '<Alt+Tab>') return [[164,1],...stroke(9),[164,0]];
    if (token === '<Ctrl>') return stroke(162);
    if (token === '<Shift>') return stroke(160);
    if (/^<Num[0-9]>$/.test(token)) return stroke(96+Number(token[4]));
    if (/^[a-z0-9 ]$/.test(token)) return stroke(token === ' ' ? 32 : token.toUpperCase().charCodeAt(0));
    throw new Error(`unsupported own live-fixture token: ${token}`);
  });
  const send = tokens => probe(env,browserPid,['--send-events',events(tokens).map(pair=>pair.join(':')).join(',')]);
  const activate = async kind => {
    const id = {input:'plain',textarea:'multi',contenteditable:'rich'}[kind];
    const g = await evaluate(`(() => { const e=document.querySelector('#${id}');e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect();return {width:innerWidth,height:innerHeight,x:r.x+r.width/2,y:r.y+r.height/2}; })()`);
    await probe(env,browserPid,['--activate-own-fixture','--viewport-width',String(g.width),'--viewport-height',String(g.height),'--point-x',String(g.x),'--point-y',String(g.y)]);
  };
  const results = [];
  for(const test of cases.filter(item=>filter===null||`${item.kind}/${item.name}`.includes(filter))) for(let attempt=1;attempt<=repeat;++attempt) {
    const initial={kind:test.kind,text:test.seed??''};
    await evaluate(`window.lshiftFixture.reset(${JSON.stringify(initial)})`);
    await activate(test.kind);
    await evaluate(`window.lshiftFixture.reset(${JSON.stringify(initial)})`);
    await probe(env,browserPid,['--set-layout',String(0x04090409)]);
    await pause(test.focusDelay??80);
    await send(test.tokens);
    if(test.focusCycle) {
      if(test.focusCycle==='mouse') {
        const other=test.kind==='input'?'textarea':'input';
        await activate(other);
      } else { await send(['<Alt+Tab>']); await pause(400); }
      await activate(test.kind);
      await send(test.then);
    }
    await pause(test.settle??60);
    const before=await evaluate('window.lshiftFixture.oracle()');
    if(test.cancelWithTab) {
      if(before.value!=='ghbdtn'||!before.focused)throw new Error('focus-cancellation setup failed: '+JSON.stringify(before));
      const nextId={input:'multi',textarea:'rich',contenteditable:'plain'}[test.kind];
      await evaluate(`(() => {const e=document.querySelector('#${nextId}');if(e.isContentEditable)e.textContent='ghbdtn';else e.value='ghbdtn';})()`);
      await send(['<Shift>','<Shift>','<Tab>']);await pause(350);
      const other=await evaluate(`(() => {const e=document.querySelector('#${nextId}');return e.isContentEditable?e.innerText:e.value;})()`);
      const current=await evaluate('window.lshiftFixture.oracle()');
      const passed=other==='ghbdtn'&&['ghbdtn','привет'].includes(current.value);
      const record={name:`${test.kind}/${test.name}`,attempt,otherValue:other,originalValue:current.value,otherFieldUntouched:other==='ghbdtn',passed};
      results.push(record);console.log(JSON.stringify(record));continue;
    }
    const expectedBefore=test.before??(test.expected.includes('привет')?test.expected.replace('привет','ghbdtn'):test.expected);
    if(before.value!==expectedBefore||!before.focused) throw new Error(`live setup failed ${test.kind}/${test.name}: ${JSON.stringify({value:before.value,expectedBefore,focused:before.focused})}`);
    await evaluate('window.lshiftFixture.beginObservation()');
    await send(['<Shift>','<Shift>']);
    await pause(350);
    const after=await evaluate('window.lshiftFixture.oracle()');
    const layout=await probe(env,browserPid,['--inspect-layout']);
    const converted=!test.unchanged;
    const record={name:`${test.kind}/${test.name}`,attempt,valueMatches:after.value===test.expected,
      caretMatches:after.start===test.expected.length&&after.end===test.expected.length,
      layoutMatches:layout.layout_after===(converted?0x04190419:0x04090409),
      noProgramSelection:Boolean(test.selected)||after.observation.nonempty===0, roundTripMatches:true};
    if(test.roundTrip&&record.valueMatches) {
      await send(['<Shift>','<Shift>']);await pause(350);
      const reverse=await evaluate('window.lshiftFixture.oracle()');
      record.roundTripMatches=reverse.value==='ghbdtn';
    }
    record.passed=record.valueMatches&&record.caretMatches&&record.layoutMatches&&record.noProgramSelection&&record.roundTripMatches;
    results.push(record);console.log(JSON.stringify(record));
  }
  return results;
}
