" Debugger plugin using gdb.
"
" Author: Bram Moolenaar
" Copyright: Vim license applies, see ":help license"
" Last Update: 2018 Jun 3
"
" WORK IN PROGRESS - Only the basics work
" Note: On MS-Windows you need a recent version of gdb.  The one included with
" MingW is too old (7.6.1).
" I used version 7.12 from http://www.equation.com/servlet/equation.cmd?fa=gdb
"
" There are two ways to run gdb:
" - In a terminal window; used if possible, does not work on MS-Windows
"   Not used when g:termdebug_use_prompt is set to 1.
" - Using a "prompt" buffer; may use a terminal window for the program
"
" For both the current window is used to view source code and shows the
" current statement from gdb.
"
" USING A TERMINAL WINDOW
"
" Opens two visible terminal windows:
" 1. runs a pty for the debugged program, as with ":term NONE"
" 2. runs gdb, passing the pty of the debugged program
" A third terminal window is hidden, it is used for communication with gdb.
"
" USING A PROMPT BUFFER
"
" Opens a window with a prompt buffer to communicate with gdb.
" Gdb is run as a job with callbacks for I/O.
" On Unix another terminal window is opened to run the debugged program
" On MS-Windows a separate console is opened to run the debugged program
"
" The communication with gdb uses GDB/MI.  See:
" https://sourceware.org/gdb/current/onlinedocs/gdb/GDB_002fMI.html

" In case this gets sourced twice.
if exists(':Termdebug')
  finish
endif

" Need either the +terminal feature or +channel and the prompt buffer.
" The terminal feature does not work with gdb on win32.
if has('terminal') && !has('win32')
  let s:way = 'terminal'
elseif has('channel') && exists('*prompt_setprompt')
  let s:way = 'prompt'
else
  if has('terminal')
    let s:err = 'Cannot debug, missing prompt buffer support'
  else
    let s:err = 'Cannot debug, +channel feature is not supported'
  endif
  command -nargs=* -complete=file -bang Termdebug echoerr s:err
  command -nargs=+ -complete=file -bang TermdebugCommand echoerr s:err
  finish
endif

let s:keepcpo = &cpo
set cpo&vim

" The command that starts debugging, e.g. ":Termdebug vim".
" To end type "quit" in the gdb window.
command -nargs=* -complete=file -bang Termdebug call s:StartDebug(<bang>0, <f-args>)
command -nargs=+ -complete=file -bang TermdebugCommand call s:StartDebugCommand(<bang>0, <f-args>)

" Name of the gdb command, defaults to "gdb".
if !exists('g:termdebugger')
  let g:termdebugger = 'gdb'
endif

let s:pc_id = 12
let s:break_id = 13  " breakpoint number is added to this
let s:stopped = 1

" Take a breakpoint number as used by GDB and turn it into an integer.
" The breakpoint may contain a dot: 123.4 -> 123004
" The main breakpoint has a zero subid.
func s:Breakpoint2SignNumber(id, subid)
  return s:break_id + a:id * 1000 + a:subid
endfunction

func s:Highlight(init, old, new)
  let default = a:init ? 'default ' : ''
  if a:new ==# 'light' && a:old !=# 'light'
    exe "hi " . default . "debugPC term=reverse ctermbg=lightblue guibg=lightblue"
  elseif a:new ==# 'dark' && a:old !=# 'dark'
    exe "hi " . default . "debugPC term=reverse ctermbg=darkblue guibg=darkblue"
  endif
endfunc

call s:Highlight(1, '', &background)
hi default debugBreakpoint term=reverse ctermbg=red guibg=red

func s:StartDebug(bang, ...)
  " First argument is the command to debug, second core file or process ID.
  call s:StartDebug_internal({'gdb_args': a:000, 'bang': a:bang})
endfunc

func s:StartDebugCommand(bang, ...)
  " First argument is the command to debug, rest are run arguments.
  call s:StartDebug_internal({'gdb_args': [a:1], 'proc_args': a:000[1:], 'bang': a:bang})
endfunc

func s:StartDebug_internal(dict)
  if exists('s:gdbwin')
    echoerr 'Terminal debugger already running, cannot run two'
    return
  endif
  if !executable(g:termdebugger)
    echoerr 'Cannot execute debugger program "' .. g:termdebugger .. '"'
    return
  endif

  let s:ptywin = 0
  let s:pid = 0

  " Uncomment this line to write logging in "debuglog".
  " call ch_logfile('debuglog', 'w')

  let s:sourcewin = win_getid(winnr())
  let s:startsigncolumn = &signcolumn

  let s:save_columns = 0
  let s:allleft = 0
  if exists('g:termdebug_wide')
    if &columns < g:termdebug_wide
      let s:save_columns = &columns
      let &columns = g:termdebug_wide
      " If we make the Vim window wider, use the whole left halve for the debug
      " windows.
      let s:allleft = 1
    endif
    let s:vertical = 1
  else
    let s:vertical = 0
  endif

  " Override using a terminal window by setting g:termdebug_use_prompt to 1.
  let use_prompt = exists('g:termdebug_use_prompt') && g:termdebug_use_prompt
  if has('terminal') && !has('win32') && !use_prompt
    let s:way = 'terminal'
  else
    let s:way = 'prompt'
  endif

  if s:way == 'prompt'
    call s:StartDebug_prompt(a:dict)
  else
    call s:StartDebug_term(a:dict)
  endif
endfunc

" Use when debugger didn't start or ended.
func s:CloseBuffers()
  exe 'bwipe! ' . s:ptybuf
  exe 'bwipe! ' . s:commbuf
  unlet! s:gdbwin
endfunc

func s:StartDebug_term(dict)
  " Open a terminal window without a job, to run the debugged program in.
  let s:ptybuf = term_start('NONE', {
	\ 'term_name': 'debugged program',
	\ 'vertical': s:vertical,
	\ })
  if s:ptybuf == 0
    echoerr 'Failed to open the program terminal window'
    return
  endif
  let pty = job_info(term_getjob(s:ptybuf))['tty_out']
  let s:ptywin = win_getid(winnr())
  if s:vertical
    " Assuming the source code window will get a signcolumn, use two more
    " columns for that, thus one less for the terminal window.
    exe (&columns / 2 - 1) . "wincmd |"
    if s:allleft
      " use the whole left column
      wincmd H
    endif
  endif

  " Create a hidden terminal window to communicate with gdb
  let s:commbuf = term_start('NONE', {
	\ 'term_name': 'gdb communication',
	\ 'out_cb': function('s:CommOutput'),
	\ 'hidden': 1,
	\ })
  if s:commbuf == 0
    echoerr 'Failed to open the communication terminal window'
    exe 'bwipe! ' . s:ptybuf
    return
  endif
  let commpty = job_info(term_getjob(s:commbuf))['tty_out']

  " Open a terminal window to run the debugger.
  " Add -quiet to avoid the intro message causing a hit-enter prompt.
  let gdb_args = get(a:dict, 'gdb_args', [])
  let proc_args = get(a:dict, 'proc_args', [])

  let cmd = [g:termdebugger, '-quiet', '-tty', pty] + gdb_args
  call ch_log('executing "' . join(cmd) . '"')
  let s:gdbbuf = term_start(cmd, {
	\ 'term_finish': 'close',
	\ })
  if s:gdbbuf == 0
    echoerr 'Failed to open the gdb terminal window'
    call s:CloseBuffers()
    return
  endif
  let s:gdbwin = win_getid(winnr())

  " Set arguments to be run
  if len(proc_args)
    call term_sendkeys(s:gdbbuf, 'set args ' . join(proc_args) . "\r")
  endif

  " Connect gdb to the communication pty, using the GDB/MI interface
  call term_sendkeys(s:gdbbuf, 'new-ui mi ' . commpty . "\r")

  " Wait for the response to show up, users may not notice the error and wonder
  " why the debugger doesn't work.
  let try_count = 0
  while 1
    let gdbproc = term_getjob(s:gdbbuf)
    if gdbproc == v:null || job_status(gdbproc) !=# 'run'
      echoerr string(g:termdebugger) . ' exited unexpectedly'
      call s:CloseBuffers()
      return
    endif

    let response = ''
    for lnum in range(1, 200)
      let line1 = term_getline(s:gdbbuf, lnum)
      let line2 = term_getline(s:gdbbuf, lnum + 1)
      if line1 =~ 'new-ui mi '
	" response can be in the same line or the next line
	let response = line1 . line2
	if response =~ 'Undefined command'
	  echoerr 'Sorry, your gdb is too old, gdb 7.12 is required'
	  call s:CloseBuffers()
	  return
	endif
	if response =~ 'New UI allocated'
	  " Success!
	  break
	endif
      elseif line1 =~ 'Reading symbols from' && line2 !~ 'new-ui mi '
	" Reading symbols might take a while, try more times
	let try_count -= 1
      endif
    endfor
    if response =~ 'New UI allocated'
      break
    endif
    let try_count += 1
    if try_count > 100
      echoerr 'Cannot check if your gdb works, continuing anyway'
      break
    endif
    sleep 10m
  endwhile

  " Interpret commands while the target is running.  This should usually only be
  " exec-interrupt, since many commands don't work properly while the target is
  " running.
  call s:SendCommand('-gdb-set mi-async on')
  " Older gdb uses a different command.
  call s:SendCommand('-gdb-set target-async on')

  " Disable pagination, it causes everything to stop at the gdb
  " "Type <return> to continue" prompt.
  call s:SendCommand('set pagination off')

  call job_setoptions(gdbproc, {'exit_cb': function('s:EndTermDebug')})
  call s:StartDebugCommon(a:dict)
endfunc

func s:StartDebug_prompt(dict)
  " Open a window with a prompt buffer to run gdb in.
  if s:vertical
    vertical new
  else
    new
  endif
  let s:gdbwin = win_getid(winnr())
  let s:promptbuf = bufnr('')
  call prompt_setprompt(s:promptbuf, 'gdb> ')
  set buftype=prompt
  file gdb
  call prompt_setcallback(s:promptbuf, function('s:PromptCallback'))
  call prompt_setinterrupt(s:promptbuf, function('s:PromptInterrupt'))

  if s:vertical
    " Assuming the source code window will get a signcolumn, use two more
    " columns for that, thus one less for the terminal window.
    exe (&columns / 2 - 1) . "wincmd |"
  endif

  " Add -quiet to avoid the intro message causing a hit-enter prompt.
  let gdb_args = get(a:dict, 'gdb_args', [])
  let proc_args = get(a:dict, 'proc_args', [])

  let cmd = [g:termdebugger, '-quiet', '--interpreter=mi2'] + gdb_args
  call ch_log('executing "' . join(cmd) . '"')

  let s:gdbjob = job_start(cmd, {
	\ 'exit_cb': function('s:EndPromptDebug'),
	\ 'out_cb': function('s:GdbOutCallback'),
	\ })
  if job_status(s:gdbjob) != "run"
    echoerr 'Failed to start gdb'
    exe 'bwipe! ' . s:promptbuf
    return
  endif
  " Mark the buffer modified so that it's not easy to close.
  set modified
  let s:gdb_channel = job_getchannel(s:gdbjob)  

  " Interpret commands while the target is running.  This should usually only
  " be exec-interrupt, since many commands don't work properly while the
  " target is running.
  call s:SendCommand('-gdb-set mi-async on')
  " Older gdb uses a different command.
  call s:SendCommand('-gdb-set target-async on')

  let s:ptybuf = 0
  if has('win32')
    " MS-Windows: run in a new console window for maximum compatibility
    call s:SendCommand('set new-console on')
  elseif has('terminal')
    " Unix: Run the debugged program in a terminal window.  Open it below the
    " gdb window.
    belowright let s:ptybuf = term_start('NONE', {
	  \ 'term_name': 'debugged program',
	  \ })
    if s:ptybuf == 0
      echoerr 'Failed to open the program terminal window'
      call job_stop(s:gdbjob)
      return
    endif
    let s:ptywin = win_getid(winnr())
    let pty = job_info(term_getjob(s:ptybuf))['tty_out']
    call s:SendCommand('tty ' . pty)

    " Since GDB runs in a prompt window, the environment has not been set to
    " match a terminal window, need to do that now.
    call s:SendCommand('set env TERM = xterm-color')
    call s:SendCommand('set env ROWS = ' . winheight(s:ptywin))
    call s:SendCommand('set env LINES = ' . winheight(s:ptywin))
    call s:SendCommand('set env COLUMNS = ' . winwidth(s:ptywin))
    call s:SendCommand('set env COLORS = ' . &t_Co)
    call s:SendCommand('set env VIM_TERMINAL = ' . v:version)
  else
    " TODO: open a new terminal get get the tty name, pass on to gdb
    call s:SendCommand('show inferior-tty')
  endif
  call s:SendCommand('set print pretty on')
  call s:SendCommand('set breakpoint pending on')
  " Disable pagination, it causes everything to stop at the gdb
  call s:SendCommand('set pagination off')

  " Set arguments to be run
  if len(proc_args)
    call s:SendCommand('set args ' . join(proc_args))
  endif

  call s:StartDebugCommon(a:dict)
  startinsert
endfunc

func s:StartDebugCommon(dict)
  " Sign used to highlight the line where the program has stopped.
  " There can be only one.
  sign define debugPC linehl=debugPC

  " Install debugger commands in the text window.
  call win_gotoid(s:sourcewin)
  call s:InstallCommands()
  call win_gotoid(s:gdbwin)

  " Enable showing a balloon with eval info
  if has("balloon_eval") || has("balloon_eval_term")
    set balloonexpr=TermDebugBalloonExpr()
    if has("balloon_eval")
      set ballooneval
    endif
    if has("balloon_eval_term")
      set balloonevalterm
    endif
  endif

  " Contains breakpoints that have been placed, key is a string with the GDB
  " breakpoint number.
  " Each entry is a dict, containing the sub-breakpoints.  Key is the subid.
  " For a breakpoint that is just a number the subid is zero.
  " For a breakpoint "123.4" the id is "123" and subid is "4".
  " Example, when breakpoint "44", "123", "123.1" and "123.2" exist:
  " {'44': {'0': entry}, '123': {'0': entry, '1': entry, '2': entry}}
  let s:breakpoints = {}

  " Contains breakpoints by file/lnum.  The key is "fname:lnum".
  " Each entry is a list of breakpoint IDs at that position.
  let s:breakpoint_locations = {}

  augroup TermDebug
    au BufRead * call s:BufRead()
    au BufUnload * call s:BufUnloaded()
    au OptionSet background call s:Highlight(0, v:option_old, v:option_new)
  augroup END

  " Run the command if the bang attribute was given and got to the debug
  " window.
  if get(a:dict, 'bang', 0)
    call s:SendCommand('-exec-run')
    call win_gotoid(s:ptywin)
  endif
endfunc

" Send a command to gdb.  "cmd" is the string without line terminator.
func s:SendCommand(cmd)
  call ch_log('sending to gdb: ' . a:cmd)
  if s:way == 'prompt'
    call ch_sendraw(s:gdb_channel, a:cmd . "\n")
  else
    call term_sendkeys(s:commbuf, a:cmd . "\r")
  endif
endfunc

" This is global so that a user can create their mappings with this.
func TermDebugSendCommand(cmd)
  if s:way == 'prompt'
    call ch_sendraw(s:gdb_channel, a:cmd . "\n")
  else
    let do_continue = 0
    if !s:stopped
      let do_continue = 1
      call s:SendCommand('-exec-interrupt')
      sleep 10m
    endif
    call term_sendkeys(s:gdbbuf, a:cmd . "\r")
    if do_continue
      Continue
    endif
  endif
endfunc

" Function called when entering a line in the prompt buffer.
func s:PromptCallback(text)
  call s:SendCommand(a:text)
endfunc

" Function called when pressing CTRL-C in the prompt buffer and when placing a
" breakpoint.
func s:PromptInterrupt()
  call ch_log('Interrupting gdb')
  if has('win32')
    " Using job_stop() does not work on MS-Windows, need to send SIGTRAP to
    " the debugger program so that gdb responds again.
    if s:pid == 0
      echoerr 'Cannot interrupt gdb, did not find a process ID'
    else
      call debugbreak(s:pid)
    endif
  else
    call job_stop(s:gdbjob, 'int')
  endif
endfunc

" Function called when gdb outputs text.
func s:GdbOutCallback(channel, text)
  call ch_log('received from gdb: ' . a:text)

  " Drop the gdb prompt, we have our own.
  " Drop status and echo'd commands.
  if a:text == '(gdb) ' || a:text == '^done' || a:text[0] == '&'
    return
  endif
  if a:text =~ '^^error,msg='
    let text = s:DecodeMessage(a:text[11:])
    if exists('s:evalexpr') && text =~ 'A syntax error in expression, near\|No symbol .* in current context'
      " Silently drop evaluation errors.
      unlet s:evalexpr
      return
    endif
  elseif a:text[0] == '~'
    let text = s:DecodeMessage(a:text[1:])
  else
    call s:CommOutput(a:channel, a:text)
    return
  endif

  let curwinid = win_getid(winnr())
  call win_gotoid(s:gdbwin)

  " Add the output above the current prompt.
  call append(line('$') - 1, text)
  set modified

  call win_gotoid(curwinid)
endfunc

" Decode a message from gdb.  quotedText starts with a ", return the text up
" to the next ", unescaping characters.
func s:DecodeMessage(quotedText)
  if a:quotedText[0] != '"'
    echoerr 'DecodeMessage(): missing quote in ' . a:quotedText
    return
  endif
  let result = ''
  let i = 1
  while a:quotedText[i] != '"' && i < len(a:quotedText)
    if a:quotedText[i] == '\'
      let i += 1
      if a:quotedText[i] == 'n'
	" drop \n
	let i += 1
	continue
      elseif a:quotedText[i] == 't'
	" append \t
	let i += 1
	let result .= "\t"
	continue
      endif
    endif
    let result .= a:quotedText[i]
    let i += 1
  endwhile
  return result
endfunc

" Extract the "name" value from a gdb message with fullname="name".
func s:GetFullname(msg)
  if a:msg !~ 'fullname'
    return ''
  endif
  let name = s:DecodeMessage(substitute(a:msg, '.*fullname=', '', ''))
  if has('win32') && name =~ ':\\\\'
    " sometimes the name arrives double-escaped
    let name = substitute(name, '\\\\', '\\', 'g')
  endif
  return name
endfunc

func s:EndTermDebug(job, status)
  exe 'bwipe! ' . s:commbuf
  unlet s:gdbwin

  call s:EndDebugCommon()
endfunc

func s:EndDebugCommon()
  let curwinid = win_getid(winnr())

  if exists('s:ptybuf') && s:ptybuf
    exe 'bwipe! ' . s:ptybuf
  endif

  call win_gotoid(s:sourcewin)
  let &signcolumn = s:startsigncolumn
  call s:DeleteCommands()

  call win_gotoid(curwinid)

  if s:save_columns > 0
    let &columns = s:save_columns
  endif

  if has("balloon_eval") || has("balloon_eval_term")
    set balloonexpr=
    if has("balloon_eval")
      set noballooneval
    endif
    if has("balloon_eval_term")
      set noballoonevalterm
    endif
  endif

  au! TermDebug
endfunc

func s:EndPromptDebug(job, status)
  let curwinid = win_getid(winnr())
  call win_gotoid(s:gdbwin)
  set nomodified
  close
  if curwinid != s:gdbwin
    call win_gotoid(curwinid)
  endif

  call s:EndDebugCommon()
  unlet s:gdbwin
  call ch_log("Returning from EndPromptDebug()")
endfunc

" Handle a message received from gdb on the GDB/MI interface.
func s:CommOutput(chan, msg)
  let msgs = split(a:msg, "\r")

  for msg in msgs
    " remove prefixed NL
    if msg[0] == "\n"
      let msg = msg[1:]
    endif
    if msg != ''
      if msg =~ '^\(\*stopped\|\*running\|=thread-selected\)'
	call s:HandleCursor(msg)
      elseif msg =~ '^\^done,bkpt=' || msg =~ '^=breakpoint-created,'
	call s:HandleNewBreakpoint(msg)
      elseif msg =~ '^=breakpoint-deleted,'
	call s:HandleBreakpointDelete(msg)
      elseif msg =~ '^=thread-group-started'
	call s:HandleProgramRun(msg)
      elseif msg =~ '^\^done,value='
	call s:HandleEvaluate(msg)
      elseif msg =~ '^\^error,msg='
	call s:HandleError(msg)
      endif
    endif
  endfor
endfunc

func s:GotoProgram()
  if has('win32')
    if executable('powershell')
      call system(printf('powershell -Command "add-type -AssemblyName microsoft.VisualBasic;[Microsoft.VisualBasic.Interaction]::AppActivate(%d);"', s:pid))
    endif
  else
    win_gotoid(s:ptywin)
  endif
endfunc

" Install commands in the current window to control the debugger.
func s:InstallCommands()
  let save_cpo = &cpo
  set cpo&vim

  command -nargs=? Break call s:SetBreakpoint(<q-args>)
  command Clear call s:ClearBreakpoint()
  command Step call s:SendCommand('-exec-step')
  command Over call s:SendCommand('-exec-next')
  command Finish call s:SendCommand('-exec-finish')
  command -nargs=* Run call s:Run(<q-args>)
  command -nargs=* Arguments call s:SendCommand('-exec-arguments ' . <q-args>)
  command Stop call s:SendCommand('-exec-interrupt')

  " using -exec-continue results in CTRL-C in gdb window not working
  if s:way == 'prompt'
    command Continue call s:SendCommand('continue')
  else
    command Continue call term_sendkeys(s:gdbbuf, "continue\r")
  endif

  command -range -nargs=* Evaluate call s:Evaluate(<range>, <q-args>)
  command Gdb call win_gotoid(s:gdbwin)
  command Program call s:GotoProgram()
  command Source call s:GotoSourcewinOrCreateIt()
  command Winbar call s:InstallWinbar()

  " TODO: can the K mapping be restored?
  nnoremap K :Evaluate<CR>

  if has('menu') && &mouse != ''
    call s:InstallWinbar()

    if !exists('g:termdebug_popup') || g:termdebug_popup != 0
      let s:saved_mousemodel = &mousemodel
      let &mousemodel = 'popup_setpos'
      an 1.200 PopUp.-SEP3-	<Nop>
      an 1.210 PopUp.Set\ breakpoint	:Break<CR>
      an 1.220 PopUp.Clear\ breakpoint	:Clear<CR>
      an 1.230 PopUp.Evaluate		:Evaluate<CR>
    endif
  endif

  let &cpo = save_cpo
endfunc

let s:winbar_winids = []

" Install the window toolbar in the current window.
func s:InstallWinbar()
  if has('menu') && &mouse != ''
    nnoremenu WinBar.Step   :Step<CR>
    nnoremenu WinBar.Next   :Over<CR>
    nnoremenu WinBar.Finish :Finish<CR>
    nnoremenu WinBar.Cont   :Continue<CR>
    nnoremenu WinBar.Stop   :Stop<CR>
    nnoremenu WinBar.Eval   :Evaluate<CR>
    call add(s:winbar_winids, win_getid(winnr()))
  endif
endfunc

" Delete installed debugger commands in the current window.
func s:DeleteCommands()
  delcommand Break
  delcommand Clear
  delcommand Step
  delcommand Over
  delcommand Finish
  delcommand Run
  delcommand Arguments
  delcommand Stop
  delcommand Continue
  delcommand Evaluate
  delcommand Gdb
  delcommand Program
  delcommand Source
  delcommand Winbar

  nunmap K

  if has('menu')
    " Remove the WinBar entries from all windows where it was added.
    let curwinid = win_getid(winnr())
    for winid in s:winbar_winids
      if win_gotoid(winid)
	aunmenu WinBar.Step
	aunmenu WinBar.Next
	aunmenu WinBar.Finish
	aunmenu WinBar.Cont
	aunmenu WinBar.Stop
	aunmenu WinBar.Eval
      endif
    endfor
    call win_gotoid(curwinid)
    let s:winbar_winids = []

    if exists('s:saved_mousemodel')
      let &mousemodel = s:saved_mousemodel
      unlet s:saved_mousemodel
      aunmenu PopUp.-SEP3-
      aunmenu PopUp.Set\ breakpoint
      aunmenu PopUp.Clear\ breakpoint
      aunmenu PopUp.Evaluate
    endif
  endif

  exe 'sign unplace ' . s:pc_id
  for [id, entries] in items(s:breakpoints)
    for subid in keys(entries)
      exe 'sign unplace ' . s:Breakpoint2SignNumber(id, subid)
    endfor
  endfor
  unlet s:breakpoints
  unlet s:breakpoint_locations

  sign undefine debugPC
  for val in s:BreakpointSigns
    exe "sign undefine debugBreakpoint" . val
  endfor
  let s:BreakpointSigns = []
endfunc

" :Break - Set a breakpoint at the cursor position.
func s:SetBreakpoint(at)
  " Setting a breakpoint may not work while the program is running.
  " Interrupt to make it work.
  let do_continue = 0
  if !s:stopped
    let do_continue = 1
    if s:way == 'prompt'
      call s:PromptInterrupt()
    else
      call s:SendCommand('-exec-interrupt')
    endif
    sleep 10m
  endif

  " Use the fname:lnum format, older gdb can't handle --source.
  let at = empty(a:at) ?
        \ fnameescape(expand('%:p')) . ':' . line('.') : a:at
  call s:SendCommand('-break-insert ' . at)
  if do_continue
    call s:SendCommand('-exec-continue')
  endif
endfunc

" :Clear - Delete a breakpoint at the cursor position.
func s:ClearBreakpoint()
  let fname = fnameescape(expand('%:p'))
  let lnum = line('.')
  let bploc = printf('%s:%d', fname, lnum)
  if has_key(s:breakpoint_locations, bploc)
    let idx = 0
    for id in s:breakpoint_locations[bploc]
      if has_key(s:breakpoints, id)
	" Assume this always works, the reply is simply "^done".
	call s:SendCommand('-break-delete ' . id)
	for subid in keys(s:breakpoints[id])
	  exe 'sign unplace ' . s:Breakpoint2SignNumber(id, subid)
	endfor
	unlet s:breakpoints[id]
	unlet s:breakpoint_locations[bploc][idx]
	break
      else
	let idx += 1
      endif
    endfor
    if empty(s:breakpoint_locations[bploc])
      unlet s:breakpoint_locations[bploc]
    endif
  endif
endfunc

func s:Run(args)
  if a:args != ''
    call s:SendCommand('-exec-arguments ' . a:args)
  endif
  call s:SendCommand('-exec-run')
endfunc

func s:SendEval(expr)
  call s:SendCommand('-data-evaluate-expression "' . a:expr . '"')
  let s:evalexpr = a:expr
endfunc

" :Evaluate - evaluate what is under the cursor
func s:Evaluate(range, arg)
  if a:arg != ''
    let expr = a:arg
  elseif a:range == 2
    let pos = getcurpos()
    let reg = getreg('v', 1, 1)
    let regt = getregtype('v')
    normal! gv"vy
    let expr = @v
    call setpos('.', pos)
    call setreg('v', reg, regt)
  else
    let expr = expand('<cexpr>')
  endif
  let s:ignoreEvalError = 0
  call s:SendEval(expr)
endfunc

let s:ignoreEvalError = 0
let s:evalFromBalloonExpr = 0

" Handle the result of data-evaluate-expression
func s:HandleEvaluate(msg)
  let value = substitute(a:msg, '.*value="\(.*\)"', '\1', '')
  let value = substitute(value, '\\"', '"', 'g')
  if s:evalFromBalloonExpr
    if s:evalFromBalloonExprResult == ''
      let s:evalFromBalloonExprResult = s:evalexpr . ': ' . value
    else
      let s:evalFromBalloonExprResult .= ' = ' . value
    endif
    call balloon_show(s:evalFromBalloonExprResult)
  else
    echomsg '"' . s:evalexpr . '": ' . value
  endif

  if s:evalexpr[0] != '*' && value =~ '^0x' && value != '0x0' && value !~ '"$'
    " Looks like a pointer, also display what it points to.
    let s:ignoreEvalError = 1
    call s:SendEval('*' . s:evalexpr)
  else
    let s:evalFromBalloonExpr = 0
  endif
endfunc

" Show a balloon with information of the variable under the mouse pointer,
" if there is any.
func TermDebugBalloonExpr()
  if v:beval_winid != s:sourcewin
    return ''
  endif
  if !s:stopped
    " Only evaluate when stopped, otherwise setting a breakpoint using the
    " mouse triggers a balloon.
    return ''
  endif
  let s:evalFromBalloonExpr = 1
  let s:evalFromBalloonExprResult = ''
  let s:ignoreEvalError = 1
  call s:SendEval(v:beval_text)
  return ''
endfunc

" Handle an error.
func s:HandleError(msg)
  if s:ignoreEvalError
    " Result of s:SendEval() failed, ignore.
    let s:ignoreEvalError = 0
    let s:evalFromBalloonExpr = 0
    return
  endif
  echoerr substitute(a:msg, '.*msg="\(.*\)"', '\1', '')
endfunc

func s:GotoSourcewinOrCreateIt()
  if !win_gotoid(s:sourcewin)
    new
    let s:sourcewin = win_getid(winnr())
    call s:InstallWinbar()
  endif
endfunc

" Handle stopping and running message from gdb.
" Will update the sign that shows the current position.
func s:HandleCursor(msg)
  let wid = win_getid(winnr())

  if a:msg =~ '^\*stopped'
    call ch_log('program stopped')
    let s:stopped = 1
  elseif a:msg =~ '^\*running'
    call ch_log('program running')
    let s:stopped = 0
  endif

  if a:msg =~ 'fullname='
    let fname = s:GetFullname(a:msg)
  else
    let fname = ''
  endif
  if a:msg =~ '^\(\*stopped\|=thread-selected\)' && filereadable(fname)
    let lnum = substitute(a:msg, '.*line="\([^"]*\)".*', '\1', '')
    if lnum =~ '^[0-9]*$'
    call s:GotoSourcewinOrCreateIt()
      if expand('%:p') != fnamemodify(fname, ':p')
	if &modified
	  " TODO: find existing window
	  exe 'split ' . fnameescape(fname)
	  let s:sourcewin = win_getid(winnr())
	  call s:InstallWinbar()
	else
	  exe 'edit ' . fnameescape(fname)
	endif
      endif
      exe lnum
      exe 'sign unplace ' . s:pc_id
      exe 'sign place ' . s:pc_id . ' line=' . lnum . ' name=debugPC file=' . fname
      setlocal signcolumn=yes
    endif
  elseif !s:stopped || fname != ''
    exe 'sign unplace ' . s:pc_id
  endif

  call win_gotoid(wid)
endfunc

let s:BreakpointSigns = []

func s:CreateBreakpoint(id, subid)
  let nr = printf('%d.%d', a:id, a:subid)
  if index(s:BreakpointSigns, nr) == -1
    call add(s:BreakpointSigns, nr)
    exe "sign define debugBreakpoint" . nr . " text=" . substitute(nr, '\..*', '', '') . " texthl=debugBreakpoint"
  endif
endfunc

func! s:SplitMsg(s)
  return split(a:s, '{.\{-}}\zs')
endfunction

" Handle setting a breakpoint
" Will update the sign that shows the breakpoint
func s:HandleNewBreakpoint(msg)
  if a:msg !~ 'fullname='
    " a watch does not have a file name
    return
  endif
  for msg in s:SplitMsg(a:msg)
    let fname = s:GetFullname(msg)
    if empty(fname)
      continue
    endif
    let nr = substitute(msg, '.*number="\([0-9.]*\)\".*', '\1', '')
    if empty(nr)
      return
    endif

    " If "nr" is 123 it becomes "123.0" and subid is "0".
    " If "nr" is 123.4 it becomes "123.4.0" and subid is "4"; "0" is discarded.
    let [id, subid; _] = map(split(nr . '.0', '\.'), 'v:val + 0')
    call s:CreateBreakpoint(id, subid)

    if has_key(s:breakpoints, id)
      let entries = s:breakpoints[id]
    else
      let entries = {}
      let s:breakpoints[id] = entries
    endif
    if has_key(entries, subid)
      let entry = entries[subid]
    else
      let entry = {}
      let entries[subid] = entry
    endif

    let lnum = substitute(msg, '.*line="\([^"]*\)".*', '\1', '')
    let entry['fname'] = fname
    let entry['lnum'] = lnum

    let bploc = printf('%s:%d', fname, lnum)
    if !has_key(s:breakpoint_locations, bploc)
      let s:breakpoint_locations[bploc] = []
    endif
    let s:breakpoint_locations[bploc] += [id]

    if bufloaded(fname)
      call s:PlaceSign(id, subid, entry)
    endif
  endfor
endfunc

func s:PlaceSign(id, subid, entry)
  let nr = printf('%d.%d', a:id, a:subid)
  exe 'sign place ' . s:Breakpoint2SignNumber(a:id, a:subid) . ' line=' . a:entry['lnum'] . ' name=debugBreakpoint' . nr . ' file=' . a:entry['fname']
  let a:entry['placed'] = 1
endfunc

" Handle deleting a breakpoint
" Will remove the sign that shows the breakpoint
func s:HandleBreakpointDelete(msg)
  let id = substitute(a:msg, '.*id="\([0-9]*\)\".*', '\1', '') + 0
  if empty(id)
    return
  endif
  if has_key(s:breakpoints, id)
    for [subid, entry] in items(s:breakpoints[id])
      if has_key(entry, 'placed')
	exe 'sign unplace ' . s:Breakpoint2SignNumber(id, subid)
	unlet entry['placed']
      endif
    endfor
    unlet s:breakpoints[id]
  endif
endfunc

" Handle the debugged program starting to run.
" Will store the process ID in s:pid
func s:HandleProgramRun(msg)
  let nr = substitute(a:msg, '.*pid="\([0-9]*\)\".*', '\1', '') + 0
  if nr == 0
    return
  endif
  let s:pid = nr
  call ch_log('Detected process ID: ' . s:pid)
endfunc

" Handle a BufRead autocommand event: place any signs.
func s:BufRead()
  let fname = expand('<afile>:p')
  for [id, entries] in items(s:breakpoints)
    for [subid, entry] in items(entries)
      if entry['fname'] == fname
	call s:PlaceSign(id, subid, entry)
      endif
    endfor
  endfor
endfunc

" Handle a BufUnloaded autocommand event: unplace any signs.
func s:BufUnloaded()
  let fname = expand('<afile>:p')
  for [id, entries] in items(s:breakpoints)
    for [subid, entry] in items(entries)
      if entry['fname'] == fname
	let entry['placed'] = 0
      endif
    endfor
  endfor
endfunc

let &cpo = s:keepcpo
unlet s:keepcpo
