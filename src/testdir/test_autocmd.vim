" Tests for autocommands

source shared.vim
source check.vim
source term_util.vim

func s:cleanup_buffers() abort
  for bnr in range(1, bufnr('$'))
    if bufloaded(bnr) && bufnr('%') != bnr
      execute 'bd! ' . bnr
    endif
  endfor
endfunc

func Test_vim_did_enter()
  call assert_false(v:vim_did_enter)

  " This script will never reach the main loop, can't check if v:vim_did_enter
  " becomes one.
endfunc

if has('timers')

  func ExitInsertMode(id)
    call feedkeys("\<Esc>")
  endfunc

  func Test_cursorhold_insert()
    " Need to move the cursor.
    call feedkeys("ggG", "xt")

    let g:triggered = 0
    au CursorHoldI * let g:triggered += 1
    set updatetime=20
    call timer_start(100, 'ExitInsertMode')
    call feedkeys('a', 'x!')
    call assert_equal(1, g:triggered)
    unlet g:triggered
    au! CursorHoldI
    set updatetime&
  endfunc

  func Test_cursorhold_insert_with_timer_interrupt()
    if !has('job')
      return
    endif
    " Need to move the cursor.
    call feedkeys("ggG", "xt")

    " Confirm the timer invoked in exit_cb of the job doesn't disturb
    " CursorHoldI event.
    let g:triggered = 0
    au CursorHoldI * let g:triggered += 1
    set updatetime=500
    call job_start(has('win32') ? 'cmd /c echo:' : 'echo',
          \ {'exit_cb': {-> timer_start(1000, 'ExitInsertMode')}})
    call feedkeys('a', 'x!')
    call assert_equal(1, g:triggered)
    unlet g:triggered
    au! CursorHoldI
    set updatetime&
  endfunc

  func Test_cursorhold_insert_ctrl_x()
    let g:triggered = 0
    au CursorHoldI * let g:triggered += 1
    set updatetime=20
    call timer_start(100, 'ExitInsertMode')
    " CursorHoldI does not trigger after CTRL-X
    call feedkeys("a\<C-X>", 'x!')
    call assert_equal(0, g:triggered)
    unlet g:triggered
    au! CursorHoldI
    set updatetime&
  endfunc

  func Test_OptionSet_modeline()
    call test_override('starting', 1)
    au! OptionSet
    augroup set_tabstop
      au OptionSet tabstop call timer_start(1, {-> execute("echo 'Handler called'", "")})
    augroup END
    call writefile(['vim: set ts=7 sw=5 :', 'something'], 'XoptionsetModeline')
    set modeline
    let v:errmsg = ''
    call assert_fails('split XoptionsetModeline', 'E12:')
    call assert_equal(7, &ts)
    call assert_equal('', v:errmsg)

    augroup set_tabstop
      au!
    augroup END
    bwipe!
    set ts&
    call delete('XoptionsetModeline')
    call test_override('starting', 0)
  endfunc

endif "has('timers')

func Test_bufunload()
  augroup test_bufunload_group
    autocmd!
    autocmd BufUnload * call add(s:li, "bufunload")
    autocmd BufDelete * call add(s:li, "bufdelete")
    autocmd BufWipeout * call add(s:li, "bufwipeout")
  augroup END

  let s:li=[]
  new
  setlocal bufhidden=
  bunload
  call assert_equal(["bufunload", "bufdelete"], s:li)

  let s:li=[]
  new
  setlocal bufhidden=delete
  bunload
  call assert_equal(["bufunload", "bufdelete"], s:li)

  let s:li=[]
  new
  setlocal bufhidden=unload
  bwipeout
  call assert_equal(["bufunload", "bufdelete", "bufwipeout"], s:li)

  au! test_bufunload_group
  augroup! test_bufunload_group
endfunc

" SEGV occurs in older versions.  (At least 7.4.2005 or older)
func Test_autocmd_bufunload_with_tabnext()
  tabedit
  tabfirst

  augroup test_autocmd_bufunload_with_tabnext_group
    autocmd!
    autocmd BufUnload <buffer> tabnext
  augroup END

  quit
  call assert_equal(2, tabpagenr('$'))

  autocmd! test_autocmd_bufunload_with_tabnext_group
  augroup! test_autocmd_bufunload_with_tabnext_group
  tablast
  quit
endfunc

func Test_autocmd_bufwinleave_with_tabfirst()
  tabedit
  augroup sample
    autocmd!
    autocmd BufWinLeave <buffer> tabfirst
  augroup END
  call setline(1, ['a', 'b', 'c'])
  edit! a.txt
  tabclose
endfunc

" SEGV occurs in older versions.  (At least 7.4.2321 or older)
func Test_autocmd_bufunload_avoiding_SEGV_01()
  split aa.txt
  let lastbuf = bufnr('$')

  augroup test_autocmd_bufunload
    autocmd!
    exe 'autocmd BufUnload <buffer> ' . (lastbuf + 1) . 'bwipeout!'
  augroup END

  " Todo: check for E937 generated first
  " call assert_fails('edit bb.txt', 'E937:')
  call assert_fails('edit bb.txt', 'E517:')

  autocmd! test_autocmd_bufunload
  augroup! test_autocmd_bufunload
  bwipe! aa.txt
  bwipe! bb.txt
endfunc

" SEGV occurs in older versions.  (At least 7.4.2321 or older)
func Test_autocmd_bufunload_avoiding_SEGV_02()
  setlocal buftype=nowrite
  let lastbuf = bufnr('$')

  augroup test_autocmd_bufunload
    autocmd!
    exe 'autocmd BufUnload <buffer> ' . (lastbuf + 1) . 'bwipeout!'
  augroup END

  normal! i1
  call assert_fails('edit a.txt', 'E517:')
  call feedkeys("\<CR>")

  autocmd! test_autocmd_bufunload
  augroup! test_autocmd_bufunload
  bwipe! a.txt
endfunc

func Test_win_tab_autocmd()
  let g:record = []

  augroup testing
    au WinNew * call add(g:record, 'WinNew')
    au WinEnter * call add(g:record, 'WinEnter') 
    au WinLeave * call add(g:record, 'WinLeave') 
    au TabNew * call add(g:record, 'TabNew')
    au TabClosed * call add(g:record, 'TabClosed')
    au TabEnter * call add(g:record, 'TabEnter')
    au TabLeave * call add(g:record, 'TabLeave')
  augroup END

  split
  tabnew
  close
  close

  call assert_equal([
	\ 'WinLeave', 'WinNew', 'WinEnter',
	\ 'WinLeave', 'TabLeave', 'WinNew', 'WinEnter', 'TabNew', 'TabEnter',
	\ 'WinLeave', 'TabLeave', 'TabClosed', 'WinEnter', 'TabEnter',
	\ 'WinLeave', 'WinEnter'
	\ ], g:record)

  let g:record = []
  tabnew somefile
  tabnext
  bwipe somefile

  call assert_equal([
	\ 'WinLeave', 'TabLeave', 'WinNew', 'WinEnter', 'TabNew', 'TabEnter',
	\ 'WinLeave', 'TabLeave', 'WinEnter', 'TabEnter',
	\ 'TabClosed'
	\ ], g:record)

  augroup testing
    au!
  augroup END
  unlet g:record
endfunc

func s:AddAnAutocmd()
  augroup vimBarTest
    au BufReadCmd * echo 'hello'
  augroup END
  call assert_equal(3, len(split(execute('au vimBarTest'), "\n")))
endfunc

func Test_early_bar()
  " test that a bar is recognized before the {event}
  call s:AddAnAutocmd()
  augroup vimBarTest | au! | augroup END
  call assert_equal(1, len(split(execute('au vimBarTest'), "\n")))

  call s:AddAnAutocmd()
  augroup vimBarTest| au!| augroup END
  call assert_equal(1, len(split(execute('au vimBarTest'), "\n")))

  " test that a bar is recognized after the {event}
  call s:AddAnAutocmd()
  augroup vimBarTest| au!BufReadCmd| augroup END
  call assert_equal(1, len(split(execute('au vimBarTest'), "\n")))

  " test that a bar is recognized after the {group}
  call s:AddAnAutocmd()
  au! vimBarTest|echo 'hello'
  call assert_equal(1, len(split(execute('au vimBarTest'), "\n")))
endfunc

func RemoveGroup()
  autocmd! StartOK
  augroup! StartOK
endfunc

func Test_augroup_warning()
  augroup TheWarning
    au VimEnter * echo 'entering'
  augroup END
  call assert_true(match(execute('au VimEnter'), "TheWarning.*VimEnter") >= 0)
  redir => res
  augroup! TheWarning
  redir END
  call assert_true(match(res, "W19:") >= 0)
  call assert_true(match(execute('au VimEnter'), "-Deleted-.*VimEnter") >= 0)

  " check "Another" does not take the pace of the deleted entry
  augroup Another
  augroup END
  call assert_true(match(execute('au VimEnter'), "-Deleted-.*VimEnter") >= 0)
  augroup! Another

  " no warning for postpone aucmd delete
  augroup StartOK
    au VimEnter * call RemoveGroup()
  augroup END
  call assert_true(match(execute('au VimEnter'), "StartOK.*VimEnter") >= 0)
  redir => res
  doautocmd VimEnter
  redir END
  call assert_true(match(res, "W19:") < 0)
  au! VimEnter
endfunc

func Test_BufReadCmdHelp()
  " This used to cause access to free memory
  au BufReadCmd * e +h
  help

  au! BufReadCmd
endfunc

func Test_BufReadCmdHelpJump()
  " This used to cause access to free memory
  au BufReadCmd * e +h{
  " } to fix highlighting
  call assert_fails('help', 'E434:')

  au! BufReadCmd
endfunc

func Test_augroup_deleted()
  " This caused a crash before E936 was introduced
  augroup x
    call assert_fails('augroup! x', 'E936:')
    au VimEnter * echo
  augroup end
  augroup! x
  call assert_true(match(execute('au VimEnter'), "-Deleted-.*VimEnter") >= 0)
  au! VimEnter
endfunc

" Tests for autocommands on :close command.
" This used to be in test13.
func Test_three_windows()
  " Clean up buffers, because in some cases this function fails.
  call s:cleanup_buffers()

  " Write three files and open them, each in a window.
  " Then go to next window, with autocommand that deletes the previous one.
  " Do this twice, writing the file.
  e! Xtestje1
  call setline(1, 'testje1')
  w
  sp Xtestje2
  call setline(1, 'testje2')
  w
  sp Xtestje3
  call setline(1, 'testje3')
  w
  wincmd w
  au WinLeave Xtestje2 bwipe
  wincmd w
  call assert_equal('Xtestje1', expand('%'))

  au WinLeave Xtestje1 bwipe Xtestje3
  close
  call assert_equal('Xtestje1', expand('%'))

  " Test deleting the buffer on a Unload event.  If this goes wrong there
  " will be the ATTENTION prompt.
  e Xtestje1
  au!
  au! BufUnload Xtestje1 bwipe
  call assert_fails('e Xtestje3', 'E937:')
  call assert_equal('Xtestje3', expand('%'))

  e Xtestje2
  sp Xtestje1
  call assert_fails('e', 'E937:')
  call assert_equal('Xtestje1', expand('%'))

  " Test changing buffers in a BufWipeout autocommand.  If this goes wrong
  " there are ml_line errors and/or a Crash.
  au!
  only
  e Xanother
  e Xtestje1
  bwipe Xtestje2
  bwipe Xtestje3
  au BufWipeout Xtestje1 buf Xtestje1
  bwipe
  call assert_equal('Xanother', expand('%'))

  only
  help
  wincmd w
  1quit
  call assert_equal('Xanother', expand('%'))

  au!
  enew
  call delete('Xtestje1')
  call delete('Xtestje2')
  call delete('Xtestje3')
endfunc

func Test_BufEnter()
  au! BufEnter
  au Bufenter * let val = val . '+'
  let g:val = ''
  split NewFile
  call assert_equal('+', g:val)
  bwipe!
  call assert_equal('++', g:val)

  " Also get BufEnter when editing a directory
  call mkdir('Xdir')
  split Xdir
  call assert_equal('+++', g:val)

  " On MS-Windows we can't edit the directory, make sure we wipe the right
  " buffer.
  bwipe! Xdir

  call delete('Xdir', 'd')
  au! BufEnter
endfunc

" Closing a window might cause an endless loop
" E814 for older Vims
func Test_autocmd_bufwipe_in_SessLoadPost()
  edit Xtest
  tabnew
  file Xsomething
  set noswapfile
  mksession!

  let content =<< trim [CODE]
    set nocp noswapfile
    let v:swapchoice="e"
    augroup test_autocmd_sessionload
    autocmd!
    autocmd SessionLoadPost * exe bufnr("Xsomething") . "bw!"
    augroup END

    func WriteErrors()
      call writefile([execute("messages")], "Xerrors")
    endfunc
    au VimLeave * call WriteErrors()
  [CODE]

  call writefile(content, 'Xvimrc')
  call system(GetVimCommand('Xvimrc') .. ' --not-a-term --noplugins -S Session.vim -c cq')
  let errors = join(readfile('Xerrors'))
  call assert_match('E814', errors)

  set swapfile
  for file in ['Session.vim', 'Xvimrc', 'Xerrors']
    call delete(file)
  endfor
endfunc

" SEGV occurs in older versions.
func Test_autocmd_bufwipe_in_SessLoadPost2()
  tabnew
  set noswapfile
  mksession!

  let content =<< trim [CODE]
    set nocp noswapfile
    function! DeleteInactiveBufs()
      tabfirst
      let tabblist = []
      for i in range(1, tabpagenr(''$''))
        call extend(tabblist, tabpagebuflist(i))
      endfor
      for b in range(1, bufnr(''$''))
        if bufexists(b) && buflisted(b) && (index(tabblist, b) == -1 || bufname(b) =~# ''^$'')
          exec ''bwipeout '' . b
        endif
      endfor
      echomsg "SessionLoadPost DONE"
    endfunction
    au SessionLoadPost * call DeleteInactiveBufs()

    func WriteErrors()
      call writefile([execute("messages")], "Xerrors")
    endfunc
    au VimLeave * call WriteErrors()
  [CODE]

  call writefile(content, 'Xvimrc')
  call system(GetVimCommand('Xvimrc') .. ' --not-a-term --noplugins -S Session.vim -c cq')
  let errors = join(readfile('Xerrors'))
  " This probably only ever matches on unix.
  call assert_notmatch('Caught deadly signal SEGV', errors)
  call assert_match('SessionLoadPost DONE', errors)

  set swapfile
  for file in ['Session.vim', 'Xvimrc', 'Xerrors']
    call delete(file)
  endfor
endfunc

func Test_empty_doau()
  doau \|
endfunc

func s:AutoCommandOptionSet(match)
  let template = "Option: <%s>, OldVal: <%s>, OldValLocal: <%s>, OldValGlobal: <%s>, NewVal: <%s>, Scope: <%s>, Command: <%s>\n"
  let item     = remove(g:options, 0)
  let expected = printf(template, item[0], item[1], item[2], item[3], item[4], item[5], item[6])
  let actual   = printf(template, a:match, v:option_old, v:option_oldlocal, v:option_oldglobal, v:option_new, v:option_type, v:option_command)
  let g:opt    = [expected, actual]
  "call assert_equal(expected, actual)
endfunc

func Test_OptionSet()
  if !has("eval") || !exists("+autochdir")
    return
  endif

  badd test_autocmd.vim

  call test_override('starting', 1)
  set nocp
  au OptionSet * :call s:AutoCommandOptionSet(expand("<amatch>"))

  " 1: Setting number option"
  let g:options=[['number', 0, 0, 0, 1, 'global', 'set']]
  set nu
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 2: Setting local number option"
  let g:options=[['number', 1, 1, '', 0, 'local', 'setlocal']]
  setlocal nonu
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 3: Setting global number option"
  let g:options=[['number', 1, '', 1, 0, 'global', 'setglobal']]
  setglobal nonu
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 4: Setting local autoindent option"
  let g:options=[['autoindent', 0, 0, '', 1, 'local', 'setlocal']]
  setlocal ai
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 5: Setting global autoindent option"
  let g:options=[['autoindent', 0, '', 0, 1, 'global', 'setglobal']]
  setglobal ai
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 6: Setting global autoindent option"
  let g:options=[['autoindent', 1, 1, 1, 0, 'global', 'set']]
  set ai!
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 6a: Setting global autoindent option"
  let g:options=[['autoindent', 1, 1, 0, 0, 'global', 'set']]
  noa setlocal ai
  noa setglobal noai
  set ai!
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " Should not print anything, use :noa
  " 7: don't trigger OptionSet"
  let g:options=[['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid']]
  noa set nonu
  call assert_equal([['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid']], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 8: Setting several global list and number option"
  let g:options=[['list', 0, 0, 0, 1, 'global', 'set'], ['number', 0, 0, 0, 1, 'global', 'set']]
  set list nu
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 9: don't trigger OptionSet"
  let g:options=[['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid'], ['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid']]
  noa set nolist nonu
  call assert_equal([['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid'], ['invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid', 'invalid']], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 10: Setting global acd"
  let g:options=[['autochdir', 0, 0, '', 1, 'local', 'setlocal']]
  setlocal acd
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 11: Setting global autoread (also sets local value)"
  let g:options=[['autoread', 0, 0, 0, 1, 'global', 'set']]
  set ar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 12: Setting local autoread"
  let g:options=[['autoread', 1, 1, '', 1, 'local', 'setlocal']]
  setlocal ar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 13: Setting global autoread"
  let g:options=[['autoread', 1, '', 1, 0, 'global', 'setglobal']]
  setglobal invar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 14: Setting option backspace through :let"
  let g:options=[['backspace', '', '', '', 'eol,indent,start', 'global', 'set']]
  let &bs="eol,indent,start"
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 15: Setting option backspace through setbufvar()"
  let g:options=[['backup', 0, 0, '', 1, 'local', 'setlocal']]
  " try twice, first time, shouldn't trigger because option name is invalid,
  " second time, it should trigger
  let bnum = bufnr('%')
  call assert_fails("call setbufvar(bnum, '&l:bk', 1)", "E355")
  " should trigger, use correct option name
  call setbufvar(bnum, '&backup', 1)
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 16: Setting number option using setwinvar"
  let g:options=[['number', 0, 0, '', 1, 'local', 'setlocal']]
  call setwinvar(0, '&number', 1)
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 17: Setting key option, shouldn't trigger"
  let g:options=[['key', 'invalid', 'invalid1', 'invalid2', 'invalid3', 'invalid4', 'invalid5']]
  setlocal key=blah
  setlocal key=
  call assert_equal([['key', 'invalid', 'invalid1', 'invalid2', 'invalid3', 'invalid4', 'invalid5']], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 18a: Setting string global option"
  let oldval = &backupext
  let g:options=[['backupext', oldval, oldval, oldval, 'foo', 'global', 'set']]
  set backupext=foo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 18b: Resetting string global option"
  let g:options=[['backupext', 'foo', 'foo', 'foo', oldval, 'global', 'set']]
  set backupext&
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 18c: Setting global string global option"
  let g:options=[['backupext', oldval, '', oldval, 'bar', 'global', 'setglobal']]
  setglobal backupext=bar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 18d: Setting local string global option"
  " As this is a global option this sets the global value even though
  " :setlocal is used!
  noa set backupext& " Reset global and local value (without triggering autocmd)
  let g:options=[['backupext', oldval, oldval, '', 'baz', 'local', 'setlocal']]
  setlocal backupext=baz
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 18e: Setting again string global option"
  noa setglobal backupext=ext_global " Reset global and local value (without triggering autocmd)
  noa setlocal backupext=ext_local " Sets the global(!) value!
  let g:options=[['backupext', 'ext_local', 'ext_local', 'ext_local', 'fuu', 'global', 'set']]
  set backupext=fuu
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 19a: Setting string local-global (to buffer) option"
  let oldval = &tags
  let g:options=[['tags', oldval, oldval, oldval, 'tagpath', 'global', 'set']]
  set tags=tagpath
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 19b: Resetting string local-global (to buffer) option"
  let g:options=[['tags', 'tagpath', 'tagpath', 'tagpath', oldval, 'global', 'set']]
  set tags&
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 19c: Setting global string local-global (to buffer) option "
  let g:options=[['tags', oldval, '', oldval, 'tagpath1', 'global', 'setglobal']]
  setglobal tags=tagpath1
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 19d: Setting local string local-global (to buffer) option"
  let g:options=[['tags', 'tagpath1', 'tagpath1', '', 'tagpath2', 'local', 'setlocal']]
  setlocal tags=tagpath2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 19e: Setting again string local-global (to buffer) option"
  " Note: v:option_old is the old global value for local-global string options
  " but the old local value for all other kinds of options.
  noa setglobal tags=tag_global " Reset global and local value (without triggering autocmd)
  noa setlocal tags=tag_local
  let g:options=[['tags', 'tag_global', 'tag_local', 'tag_global', 'tagpath', 'global', 'set']]
  set tags=tagpath
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 19f: Setting string local-global (to buffer) option to an empty string"
  " Note: v:option_old is the old global value for local-global string options
  " but the old local value for all other kinds of options.
  noa set tags=tag_global " Reset global and local value (without triggering autocmd)
  noa setlocal tags= " empty string
  let g:options=[['tags', 'tag_global', '', 'tag_global', 'tagpath', 'global', 'set']]
  set tags=tagpath
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 20a: Setting string local (to buffer) option"
  let oldval = &spelllang
  let g:options=[['spelllang', oldval, oldval, oldval, 'elvish,klingon', 'global', 'set']]
  set spelllang=elvish,klingon
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 20b: Resetting string local (to buffer) option"
  let g:options=[['spelllang', 'elvish,klingon', 'elvish,klingon', 'elvish,klingon', oldval, 'global', 'set']]
  set spelllang&
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 20c: Setting global string local (to buffer) option"
  let g:options=[['spelllang', oldval, '', oldval, 'elvish', 'global', 'setglobal']]
  setglobal spelllang=elvish
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 20d: Setting local string local (to buffer) option"
  noa set spelllang& " Reset global and local value (without triggering autocmd)
  let g:options=[['spelllang', oldval, oldval, '', 'klingon', 'local', 'setlocal']]
  setlocal spelllang=klingon
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 20e: Setting again string local (to buffer) option"
  " Note: v:option_old is the old global value for local-global string options
  " but the old local value for all other kinds of options.
  noa setglobal spelllang=spellglobal " Reset global and local value (without triggering autocmd)
  noa setlocal spelllang=spelllocal
  let g:options=[['spelllang', 'spelllocal', 'spelllocal', 'spellglobal', 'foo', 'global', 'set']]
  set spelllang=foo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 21a: Setting string local-global (to window) option"
  let oldval = &statusline
  let g:options=[['statusline', oldval, oldval, oldval, 'foo', 'global', 'set']]
  set statusline=foo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 21b: Resetting string local-global (to window) option"
  " Note: v:option_old is the old global value for local-global string options
  " but the old local value for all other kinds of options.
  let g:options=[['statusline', 'foo', 'foo', 'foo', oldval, 'global', 'set']]
  set statusline&
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 21c: Setting global string local-global (to window) option"
  let g:options=[['statusline', oldval, '', oldval, 'bar', 'global', 'setglobal']]
  setglobal statusline=bar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 21d: Setting local string local-global (to window) option"
  noa set statusline& " Reset global and local value (without triggering autocmd)
  let g:options=[['statusline', oldval, oldval, '', 'baz', 'local', 'setlocal']]
  setlocal statusline=baz
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 21e: Setting again string local-global (to window) option"
  " Note: v:option_old is the old global value for local-global string options
  " but the old local value for all other kinds of options.
  noa setglobal statusline=bar " Reset global and local value (without triggering autocmd)
  noa setlocal statusline=baz
  let g:options=[['statusline', 'bar', 'baz', 'bar', 'foo', 'global', 'set']]
  set statusline=foo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 22a: Setting string local (to window) option"
  let oldval = &foldignore
  let g:options=[['foldignore', oldval, oldval, oldval, 'fo', 'global', 'set']]
  set foldignore=fo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 22b: Resetting string local (to window) option"
  let g:options=[['foldignore', 'fo', 'fo', 'fo', oldval, 'global', 'set']]
  set foldignore&
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 22c: Setting global string local (to window) option"
  let g:options=[['foldignore', oldval, '', oldval, 'bar', 'global', 'setglobal']]
  setglobal foldignore=bar
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 22d: Setting local string local (to window) option"
  noa set foldignore& " Reset global and local value (without triggering autocmd)
  let g:options=[['foldignore', oldval, oldval, '', 'baz', 'local', 'setlocal']]
  setlocal foldignore=baz
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 22e: Setting again string local (to window) option"
  noa setglobal foldignore=glob " Reset global and local value (without triggering autocmd)
  noa setlocal foldignore=loc
  let g:options=[['foldignore', 'loc', 'loc', 'glob', 'fo', 'global', 'set']]
  set foldignore=fo
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 23a: Setting global number local option"
  noa setglobal cmdheight=8 " Reset global and local value (without triggering autocmd)
  noa setlocal cmdheight=1 " Sets the global(!) value!
  let g:options=[['cmdheight', '1', '', '1', '2', 'global', 'setglobal']]
  setglobal cmdheight=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 23b: Setting local number global option"
  noa setglobal cmdheight=8 " Reset global and local value (without triggering autocmd)
  noa setlocal cmdheight=1 " Sets the global(!) value!
  let g:options=[['cmdheight', '1', '1', '', '2', 'local', 'setlocal']]
  setlocal cmdheight=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 23c: Setting again number global option"
  noa setglobal cmdheight=8 " Reset global and local value (without triggering autocmd)
  noa setlocal cmdheight=1 " Sets the global(!) value!
  let g:options=[['cmdheight', '1', '1', '1', '2', 'global', 'set']]
  set cmdheight=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 23d: Setting again number global option"
  noa set cmdheight=8 " Reset global and local value (without triggering autocmd)
  let g:options=[['cmdheight', '8', '8', '8', '2', 'global', 'set']]
  set cmdheight=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 24a: Setting global number global-local (to buffer) option"
  noa setglobal undolevels=8 " Reset global and local value (without triggering autocmd)
  noa setlocal undolevels=1
  let g:options=[['undolevels', '8', '', '8', '2', 'global', 'setglobal']]
  setglobal undolevels=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 24b: Setting local number global-local (to buffer) option"
  noa setglobal undolevels=8 " Reset global and local value (without triggering autocmd)
  noa setlocal undolevels=1
  let g:options=[['undolevels', '1', '1', '', '2', 'local', 'setlocal']]
  setlocal undolevels=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 24c: Setting again number global-local (to buffer) option"
  noa setglobal undolevels=8 " Reset global and local value (without triggering autocmd)
  noa setlocal undolevels=1
  let g:options=[['undolevels', '1', '1', '8', '2', 'global', 'set']]
  set undolevels=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 24d: Setting again global number global-local (to buffer) option"
  noa set undolevels=8 " Reset global and local value (without triggering autocmd)
  let g:options=[['undolevels', '8', '8', '8', '2', 'global', 'set']]
  set undolevels=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 25a: Setting global number local (to buffer) option"
  noa setglobal wrapmargin=8 " Reset global and local value (without triggering autocmd)
  noa setlocal wrapmargin=1
  let g:options=[['wrapmargin', '8', '', '8', '2', 'global', 'setglobal']]
  setglobal wrapmargin=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 25b: Setting local number local (to buffer) option"
  noa setglobal wrapmargin=8 " Reset global and local value (without triggering autocmd)
  noa setlocal wrapmargin=1
  let g:options=[['wrapmargin', '1', '1', '', '2', 'local', 'setlocal']]
  setlocal wrapmargin=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 25c: Setting again number local (to buffer) option"
  noa setglobal wrapmargin=8 " Reset global and local value (without triggering autocmd)
  noa setlocal wrapmargin=1
  let g:options=[['wrapmargin', '1', '1', '8', '2', 'global', 'set']]
  set wrapmargin=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 25d: Setting again global number local (to buffer) option"
  noa set wrapmargin=8 " Reset global and local value (without triggering autocmd)
  let g:options=[['wrapmargin', '8', '8', '8', '2', 'global', 'set']]
  set wrapmargin=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 26: Setting number global-local (to window) option.
  " Such option does currently not exist.


  " 27a: Setting global number local (to window) option"
  noa setglobal foldcolumn=8 " Reset global and local value (without triggering autocmd)
  noa setlocal foldcolumn=1
  let g:options=[['foldcolumn', '8', '', '8', '2', 'global', 'setglobal']]
  setglobal foldcolumn=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 27b: Setting local number local (to window) option"
  noa setglobal foldcolumn=8 " Reset global and local value (without triggering autocmd)
  noa setlocal foldcolumn=1
  let g:options=[['foldcolumn', '1', '1', '', '2', 'local', 'setlocal']]
  setlocal foldcolumn=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 27c: Setting again number local (to window) option"
  noa setglobal foldcolumn=8 " Reset global and local value (without triggering autocmd)
  noa setlocal foldcolumn=1
  let g:options=[['foldcolumn', '1', '1', '8', '2', 'global', 'set']]
  set foldcolumn=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 27d: Ssettin again global number local (to window) option"
  noa set foldcolumn=8 " Reset global and local value (without triggering autocmd)
  let g:options=[['foldcolumn', '8', '8', '8', '2', 'global', 'set']]
  set foldcolumn=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 28a: Setting global boolean global option"
  noa setglobal nowrapscan " Reset global and local value (without triggering autocmd)
  noa setlocal wrapscan " Sets the global(!) value!
  let g:options=[['wrapscan', '1', '', '1', '0', 'global', 'setglobal']]
  setglobal nowrapscan
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 28b: Setting local boolean global option"
  noa setglobal nowrapscan " Reset global and local value (without triggering autocmd)
  noa setlocal wrapscan " Sets the global(!) value!
  let g:options=[['wrapscan', '1', '1', '', '0', 'local', 'setlocal']]
  setlocal nowrapscan
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 28c: Setting again boolean global option"
  noa setglobal nowrapscan " Reset global and local value (without triggering autocmd)
  noa setlocal wrapscan " Sets the global(!) value!
  let g:options=[['wrapscan', '1', '1', '1', '0', 'global', 'set']]
  set nowrapscan
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 28d: Setting again global boolean global option"
  noa set nowrapscan " Reset global and local value (without triggering autocmd)
  let g:options=[['wrapscan', '0', '0', '0', '1', 'global', 'set']]
  set wrapscan
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 29a: Setting global boolean global-local (to buffer) option"
  noa setglobal noautoread " Reset global and local value (without triggering autocmd)
  noa setlocal autoread
  let g:options=[['autoread', '0', '', '0', '1', 'global', 'setglobal']]
  setglobal autoread
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 29b: Setting local boolean global-local (to buffer) option"
  noa setglobal noautoread " Reset global and local value (without triggering autocmd)
  noa setlocal autoread
  let g:options=[['autoread', '1', '1', '', '0', 'local', 'setlocal']]
  setlocal noautoread
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 29c: Setting again boolean global-local (to buffer) option"
  noa setglobal noautoread " Reset global and local value (without triggering autocmd)
  noa setlocal autoread
  let g:options=[['autoread', '1', '1', '0', '1', 'global', 'set']]
  set autoread
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 29d: Setting again global boolean global-local (to buffer) option"
  noa set noautoread " Reset global and local value (without triggering autocmd)
  let g:options=[['autoread', '0', '0', '0', '1', 'global', 'set']]
  set autoread
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 30a: Setting global boolean local (to buffer) option"
  noa setglobal nocindent " Reset global and local value (without triggering autocmd)
  noa setlocal cindent
  let g:options=[['cindent', '0', '', '0', '1', 'global', 'setglobal']]
  setglobal cindent
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 30b: Setting local boolean local (to buffer) option"
  noa setglobal nocindent " Reset global and local value (without triggering autocmd)
  noa setlocal cindent
  let g:options=[['cindent', '1', '1', '', '0', 'local', 'setlocal']]
  setlocal nocindent
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 30c: Setting again boolean local (to buffer) option"
  noa setglobal nocindent " Reset global and local value (without triggering autocmd)
  noa setlocal cindent
  let g:options=[['cindent', '1', '1', '0', '1', 'global', 'set']]
  set cindent
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 30d: Setting again global boolean local (to buffer) option"
  noa set nocindent " Reset global and local value (without triggering autocmd)
  let g:options=[['cindent', '0', '0', '0', '1', 'global', 'set']]
  set cindent
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 31: Setting boolean global-local (to window) option
  " Currently no such option exists.


  " 32a: Setting global boolean local (to window) option"
  noa setglobal nocursorcolumn " Reset global and local value (without triggering autocmd)
  noa setlocal cursorcolumn
  let g:options=[['cursorcolumn', '0', '', '0', '1', 'global', 'setglobal']]
  setglobal cursorcolumn
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 32b: Setting local boolean local (to window) option"
  noa setglobal nocursorcolumn " Reset global and local value (without triggering autocmd)
  noa setlocal cursorcolumn
  let g:options=[['cursorcolumn', '1', '1', '', '0', 'local', 'setlocal']]
  setlocal nocursorcolumn
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 32c: Setting again boolean local (to window) option"
  noa setglobal nocursorcolumn " Reset global and local value (without triggering autocmd)
  noa setlocal cursorcolumn
  let g:options=[['cursorcolumn', '1', '1', '0', '1', 'global', 'set']]
  set cursorcolumn
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])

  " 32d: Setting again global boolean local (to window) option"
  noa set nocursorcolumn " Reset global and local value (without triggering autocmd)
  let g:options=[['cursorcolumn', '0', '0', '0', '1', 'global', 'set']]
  set cursorcolumn
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " 33: Test autocommands when an option value is converted internally.
  noa set backspace=1 " Reset global and local value (without triggering autocmd)
  let g:options=[['backspace', 'indent,eol', 'indent,eol', 'indent,eol', '2', 'global', 'set']]
  set backspace=2
  call assert_equal([], g:options)
  call assert_equal(g:opt[0], g:opt[1])


  " Cleanup
  au! OptionSet
  " set tags&
  for opt in ['nu', 'ai', 'acd', 'ar', 'bs', 'backup', 'cul', 'cp', 'backupext', 'tags', 'spelllang', 'statusline', 'foldignore', 'cmdheight', 'undolevels', 'wrapmargin', 'foldcolumn', 'wrapscan', 'autoread', 'cindent', 'cursorcolumn']
    exe printf(":set %s&vim", opt)
  endfor
  call test_override('starting', 0)
  delfunc! AutoCommandOptionSet
endfunc

func Test_OptionSet_diffmode()
  call test_override('starting', 1)
  " 18: Changing an option when entering diff mode
  new
  au OptionSet diff :let &l:cul=v:option_new

  call setline(1, ['buffer 1', 'line2', 'line3', 'line4'])
  call assert_equal(0, &l:cul)
  diffthis
  call assert_equal(1, &l:cul)

  vnew
  call setline(1, ['buffer 2', 'line 2', 'line 3', 'line4'])
  call assert_equal(0, &l:cul)
  diffthis
  call assert_equal(1, &l:cul)

  diffoff
  call assert_equal(0, &l:cul)
  call assert_equal(1, getwinvar(2, '&l:cul'))
  bw!

  call assert_equal(1, &l:cul)
  diffoff!
  call assert_equal(0, &l:cul)
  call assert_equal(0, getwinvar(1, '&l:cul'))
  bw!

  " Cleanup
  au! OptionSet
  call test_override('starting', 0)
endfunc

func Test_OptionSet_diffmode_close()
  call test_override('starting', 1)
  " 19: Try to close the current window when entering diff mode
  " should not segfault
  new
  au OptionSet diff close

  call setline(1, ['buffer 1', 'line2', 'line3', 'line4'])
  call assert_fails(':diffthis', 'E788')
  call assert_equal(1, &diff)
  vnew
  call setline(1, ['buffer 2', 'line 2', 'line 3', 'line4'])
  call assert_fails(':diffthis', 'E788')
  call assert_equal(1, &diff)
  set diffopt-=closeoff
  bw!
  call assert_fails(':diffoff!', 'E788')
  bw!

  " Cleanup
  au! OptionSet
  call test_override('starting', 0)
  "delfunc! AutoCommandOptionSet
endfunc

" Test for Bufleave autocommand that deletes the buffer we are about to edit.
func Test_BufleaveWithDelete()
  new | edit Xfile1

  augroup test_bufleavewithdelete
      autocmd!
      autocmd BufLeave Xfile1 bwipe Xfile2
  augroup END

  call assert_fails('edit Xfile2', 'E143:')
  call assert_equal('Xfile1', bufname('%'))

  autocmd! test_bufleavewithdelete BufLeave Xfile1
  augroup! test_bufleavewithdelete

  new
  bwipe! Xfile1
endfunc

" Test for autocommand that changes the buffer list, when doing ":ball".
func Test_Acmd_BufAll()
  enew!
  %bwipe!
  call writefile(['Test file Xxx1'], 'Xxx1')
  call writefile(['Test file Xxx2'], 'Xxx2')
  call writefile(['Test file Xxx3'], 'Xxx3')

  " Add three files to the buffer list
  split Xxx1
  close
  split Xxx2
  close
  split Xxx3
  close

  " Wipe the buffer when the buffer is opened
  au BufReadPost Xxx2 bwipe

  call append(0, 'Test file Xxx4')
  ball

  call assert_equal(2, winnr('$'))
  call assert_equal('Xxx1', bufname(winbufnr(winnr('$'))))
  wincmd t

  au! BufReadPost
  %bwipe!
  call delete('Xxx1')
  call delete('Xxx2')
  call delete('Xxx3')
  enew! | only
endfunc

" Test for autocommand that changes current buffer on BufEnter event.
" Check if modelines are interpreted for the correct buffer.
func Test_Acmd_BufEnter()
  %bwipe!
  call writefile(['start of test file Xxx1',
	      \ "\<Tab>this is a test",
	      \ 'end of test file Xxx1'], 'Xxx1')
  call writefile(['start of test file Xxx2',
	      \ 'vim: set noai :',
	      \ "\<Tab>this is a test",
	      \ 'end of test file Xxx2'], 'Xxx2')

  au BufEnter Xxx2 brew
  set ai modeline modelines=3
  edit Xxx1
  " edit Xxx2, autocmd will do :brew
  edit Xxx2
  exe "normal G?this is a\<CR>"
  " Append text with autoindent to this file
  normal othis should be auto-indented
  call assert_equal("\<Tab>this should be auto-indented", getline('.'))
  call assert_equal(3, line('.'))
  " Remove autocmd and edit Xxx2 again
  au! BufEnter Xxx2
  buf! Xxx2
  exe "normal G?this is a\<CR>"
  " append text without autoindent to Xxx
  normal othis should be in column 1
  call assert_equal("this should be in column 1", getline('.'))
  call assert_equal(4, line('.'))

  %bwipe!
  call delete('Xxx1')
  call delete('Xxx2')
  set ai&vim modeline&vim modelines&vim
endfunc

" Test for issue #57
" do not move cursor on <c-o> when autoindent is set
func Test_ai_CTRL_O()
  enew!
  set ai
  let save_fo = &fo
  set fo+=r
  exe "normal o# abcdef\<Esc>2hi\<CR>\<C-O>d0\<Esc>"
  exe "normal o# abcdef\<Esc>2hi\<C-O>d0\<Esc>"
  call assert_equal(['# abc', 'def', 'def'], getline(2, 4))

  set ai&vim
  let &fo = save_fo
  enew!
endfunc

" Test for autocommand that deletes the current buffer on BufLeave event.
" Also test deleting the last buffer, should give a new, empty buffer.
func Test_BufLeave_Wipe()
  %bwipe!
  let content = ['start of test file Xxx',
	      \ 'this is a test',
	      \ 'end of test file Xxx']
  call writefile(content, 'Xxx1')
  call writefile(content, 'Xxx2')

  au BufLeave Xxx2 bwipe
  edit Xxx1
  split Xxx2
  " delete buffer Xxx2, we should be back to Xxx1
  bwipe
  call assert_equal('Xxx1', bufname('%'))
  call assert_equal(1, winnr('$'))

  " Create an alternate buffer
  %write! test.out
  call assert_equal('test.out', bufname('#'))
  " delete alternate buffer
  bwipe test.out
  call assert_equal('Xxx1', bufname('%'))
  call assert_equal('', bufname('#'))

  au BufLeave Xxx1 bwipe
  " delete current buffer, get an empty one
  bwipe!
  call assert_equal(1, line('$'))
  call assert_equal('', bufname('%'))
  let g:bufinfo = getbufinfo()
  call assert_equal(1, len(g:bufinfo))

  call delete('Xxx1')
  call delete('Xxx2')
  call delete('test.out')
  %bwipe
  au! BufLeave

  " check that bufinfo doesn't contain a pointer to freed memory
  call test_garbagecollect_now()
endfunc

func Test_QuitPre()
  edit Xfoo
  let winid = win_getid(winnr())
  split Xbar
  au! QuitPre * let g:afile = expand('<afile>')
  " Close the other window, <afile> should be correct.
  exe win_id2win(winid) . 'q'
  call assert_equal('Xfoo', g:afile)
 
  unlet g:afile
  bwipe Xfoo
  bwipe Xbar
endfunc

func Test_Cmdline()
  au! CmdlineChanged : let g:text = getcmdline()
  let g:text = 0
  call feedkeys(":echom 'hello'\<CR>", 'xt')
  call assert_equal("echom 'hello'", g:text)
  au! CmdlineChanged

  au! CmdlineChanged : let g:entered = expand('<afile>')
  let g:entered = 0
  call feedkeys(":echom 'hello'\<CR>", 'xt')
  call assert_equal(':', g:entered)
  au! CmdlineChanged

  au! CmdlineEnter : let g:entered = expand('<afile>')
  au! CmdlineLeave : let g:left = expand('<afile>')
  let g:entered = 0
  let g:left = 0
  call feedkeys(":echo 'hello'\<CR>", 'xt')
  call assert_equal(':', g:entered)
  call assert_equal(':', g:left)
  au! CmdlineEnter
  au! CmdlineLeave

  let save_shellslash = &shellslash
  set noshellslash
  au! CmdlineEnter / let g:entered = expand('<afile>')
  au! CmdlineLeave / let g:left = expand('<afile>')
  let g:entered = 0
  let g:left = 0
  new
  call setline(1, 'hello')
  call feedkeys("/hello\<CR>", 'xt')
  call assert_equal('/', g:entered)
  call assert_equal('/', g:left)
  bwipe!
  au! CmdlineEnter
  au! CmdlineLeave
  let &shellslash = save_shellslash
endfunc

" Test for BufWritePre autocommand that deletes or unloads the buffer.
func Test_BufWritePre()
  %bwipe
  au BufWritePre Xxx1 bunload
  au BufWritePre Xxx2 bwipe

  call writefile(['start of Xxx1', 'test', 'end of Xxx1'], 'Xxx1')
  call writefile(['start of Xxx2', 'test', 'end of Xxx2'], 'Xxx2')

  edit Xtest
  e! Xxx2
  bdel Xtest
  e Xxx1
  " write it, will unload it and give an error msg
  call assert_fails('w', 'E203')
  call assert_equal('Xxx2', bufname('%'))
  edit Xtest
  e! Xxx2
  bwipe Xtest
  " write it, will delete the buffer and give an error msg
  call assert_fails('w', 'E203')
  call assert_equal('Xxx1', bufname('%'))
  au! BufWritePre
  call delete('Xxx1')
  call delete('Xxx2')
endfunc

" Test for BufUnload autocommand that unloads all the other buffers
func Test_bufunload_all()
  call writefile(['Test file Xxx1'], 'Xxx1')"
  call writefile(['Test file Xxx2'], 'Xxx2')"

  let content =<< trim [CODE]
    func UnloadAllBufs()
      let i = 1
      while i <= bufnr('$')
        if i != bufnr('%') && bufloaded(i)
          exe  i . 'bunload'
        endif
        let i += 1
      endwhile
    endfunc
    au BufUnload * call UnloadAllBufs()
    au VimLeave * call writefile(['Test Finished'], 'Xout')
    edit Xxx1
    split Xxx2
    q
  [CODE]

  call writefile(content, 'Xtest')

  call delete('Xout')
  call system(GetVimCommandClean() .. ' -N --not-a-term -S Xtest')
  call assert_true(filereadable('Xout'))

  call delete('Xxx1')
  call delete('Xxx2')
  call delete('Xtest')
  call delete('Xout')
endfunc

" Some tests for buffer-local autocommands
func Test_buflocal_autocmd()
  let g:bname = ''
  edit xx
  au BufLeave <buffer> let g:bname = expand("%")
  " here, autocommand for xx should trigger.
  " but autocommand shall not apply to buffer named <buffer>.
  edit somefile
  call assert_equal('xx', g:bname)
  let g:bname = ''
  " here, autocommand shall be auto-deleted
  bwipe xx
  " autocmd should not trigger
  edit xx
  call assert_equal('', g:bname)
  " autocmd should not trigger
  edit somefile
  call assert_equal('', g:bname)
  enew
  unlet g:bname
endfunc

" Test for "*Cmd" autocommands
func Test_Cmd_Autocmds()
  call writefile(['start of Xxx', "\tabc2", 'end of Xxx'], 'Xxx')

  enew!
  au BufReadCmd XtestA 0r Xxx|$del
  edit XtestA			" will read text of Xxd instead
  call assert_equal('start of Xxx', getline(1))

  au BufWriteCmd XtestA call append(line("$"), "write")
  write				" will append a line to the file
  call assert_equal('write', getline('$'))
  call assert_fails('read XtestA', 'E484')	" should not read anything
  call assert_equal('write', getline(4))

  " now we have:
  " 1	start of Xxx
  " 2		abc2
  " 3	end of Xxx
  " 4	write

  au FileReadCmd XtestB '[r Xxx
  2r XtestB			" will read Xxx below line 2 instead
  call assert_equal('start of Xxx', getline(3))

  " now we have:
  " 1	start of Xxx
  " 2		abc2
  " 3	start of Xxx
  " 4		abc2
  " 5	end of Xxx
  " 6	end of Xxx
  " 7	write

  au FileWriteCmd XtestC '[,']copy $
  normal 4GA1
  4,5w XtestC			" will copy lines 4 and 5 to the end
  call assert_equal("\tabc21", getline(8))
  call assert_fails('r XtestC', 'E484')	" should not read anything
  call assert_equal("end of Xxx", getline(9))

  " now we have:
  " 1	start of Xxx
  " 2		abc2
  " 3	start of Xxx
  " 4		abc21
  " 5	end of Xxx
  " 6	end of Xxx
  " 7	write
  " 8		abc21
  " 9	end of Xxx

  let g:lines = []
  au FileAppendCmd XtestD call extend(g:lines, getline(line("'["), line("']")))
  w >>XtestD			" will add lines to 'lines'
  call assert_equal(9, len(g:lines))
  call assert_fails('$r XtestD', 'E484')	" should not read anything
  call assert_equal(9, line('$'))
  call assert_equal('end of Xxx', getline('$'))

  au BufReadCmd XtestE 0r Xxx|$del
  sp XtestE			" split window with test.out
  call assert_equal('end of Xxx', getline(3))

  let g:lines = []
  exe "normal 2Goasdf\<Esc>\<C-W>\<C-W>"
  au BufWriteCmd XtestE call extend(g:lines, getline(0, '$'))
  wall				" will write other window to 'lines'
  call assert_equal(4, len(g:lines), g:lines)
  call assert_equal('asdf', g:lines[2])

  au! BufReadCmd
  au! BufWriteCmd
  au! FileReadCmd
  au! FileWriteCmd
  au! FileAppendCmd
  %bwipe!
  call delete('Xxx')
  enew!
endfunc

func SetChangeMarks(start, end)
  exe a:start. 'mark ['
  exe a:end. 'mark ]'
endfunc

" Verify the effects of autocmds on '[ and ']
func Test_change_mark_in_autocmds()
  edit! Xtest
  call feedkeys("ia\<CR>b\<CR>c\<CR>d\<C-g>u", 'xtn')

  call SetChangeMarks(2, 3)
  write
  call assert_equal([1, 4], [line("'["), line("']")])

  call SetChangeMarks(2, 3)
  au BufWritePre * call assert_equal([1, 4], [line("'["), line("']")])
  write
  au! BufWritePre

  if executable('cat')
    write XtestFilter
    write >> XtestFilter

    call SetChangeMarks(2, 3)
    " Marks are set to the entire range of the write
    au FilterWritePre * call assert_equal([1, 4], [line("'["), line("']")])
    " '[ is adjusted to just before the line that will receive the filtered
    " data
    au FilterReadPre * call assert_equal([4, 4], [line("'["), line("']")])
    " The filtered data is read into the buffer, and the source lines are
    " still present, so the range is after the source lines
    au FilterReadPost * call assert_equal([5, 12], [line("'["), line("']")])
    %!cat XtestFilter
    " After the filtered data is read, the original lines are deleted
    call assert_equal([1, 8], [line("'["), line("']")])
    au! FilterWritePre,FilterReadPre,FilterReadPost
    undo

    call SetChangeMarks(1, 4)
    au FilterWritePre * call assert_equal([2, 3], [line("'["), line("']")])
    au FilterReadPre * call assert_equal([3, 3], [line("'["), line("']")])
    au FilterReadPost * call assert_equal([4, 11], [line("'["), line("']")])
    2,3!cat XtestFilter
    call assert_equal([2, 9], [line("'["), line("']")])
    au! FilterWritePre,FilterReadPre,FilterReadPost
    undo

    call delete('XtestFilter')
  endif

  call SetChangeMarks(1, 4)
  au FileWritePre * call assert_equal([2, 3], [line("'["), line("']")])
  2,3write Xtest2
  au! FileWritePre

  call SetChangeMarks(2, 3)
  au FileAppendPre * call assert_equal([1, 4], [line("'["), line("']")])
  write >> Xtest2
  au! FileAppendPre

  call SetChangeMarks(1, 4)
  au FileAppendPre * call assert_equal([2, 3], [line("'["), line("']")])
  2,3write >> Xtest2
  au! FileAppendPre

  call SetChangeMarks(1, 1)
  au FileReadPre * call assert_equal([3, 1], [line("'["), line("']")])
  au FileReadPost * call assert_equal([4, 11], [line("'["), line("']")])
  3read Xtest2
  au! FileReadPre,FileReadPost
  undo

  call SetChangeMarks(4, 4)
  " When the line is 0, it's adjusted to 1
  au FileReadPre * call assert_equal([1, 4], [line("'["), line("']")])
  au FileReadPost * call assert_equal([1, 8], [line("'["), line("']")])
  0read Xtest2
  au! FileReadPre,FileReadPost
  undo

  call SetChangeMarks(4, 4)
  " When the line is 0, it's adjusted to 1
  au FileReadPre * call assert_equal([1, 4], [line("'["), line("']")])
  au FileReadPost * call assert_equal([2, 9], [line("'["), line("']")])
  1read Xtest2
  au! FileReadPre,FileReadPost
  undo

  bwipe!
  call delete('Xtest')
  call delete('Xtest2')
endfunc

func Test_Filter_noshelltemp()
  if !executable('cat')
    return
  endif

  enew!
  call setline(1, ['a', 'b', 'c', 'd'])

  let shelltemp = &shelltemp
  set shelltemp

  let g:filter_au = 0
  au FilterWritePre * let g:filter_au += 1
  au FilterReadPre * let g:filter_au += 1
  au FilterReadPost * let g:filter_au += 1
  %!cat
  call assert_equal(3, g:filter_au)

  if has('filterpipe')
    set noshelltemp

    let g:filter_au = 0
    au FilterWritePre * let g:filter_au += 1
    au FilterReadPre * let g:filter_au += 1
    au FilterReadPost * let g:filter_au += 1
    %!cat
    call assert_equal(0, g:filter_au)
  endif

  au! FilterWritePre,FilterReadPre,FilterReadPost
  let &shelltemp = shelltemp
  bwipe!
endfunc

func Test_TextYankPost()
  enew!
  call setline(1, ['foo'])

  let g:event = []
  au TextYankPost * let g:event = copy(v:event)

  call assert_equal({}, v:event)
  call assert_fails('let v:event = {}', 'E46:')
  call assert_fails('let v:event.mykey = 0', 'E742:')

  norm "ayiw
  call assert_equal(
    \{'regcontents': ['foo'], 'regname': 'a', 'operator': 'y', 'regtype': 'v'},
    \g:event)
  norm y_
  call assert_equal(
    \{'regcontents': ['foo'], 'regname': '',  'operator': 'y', 'regtype': 'V'},
    \g:event)
  call feedkeys("\<C-V>y", 'x')
  call assert_equal(
    \{'regcontents': ['f'], 'regname': '',  'operator': 'y', 'regtype': "\x161"},
    \g:event)
  norm "xciwbar
  call assert_equal(
    \{'regcontents': ['foo'], 'regname': 'x', 'operator': 'c', 'regtype': 'v'},
    \g:event)
  norm "bdiw
  call assert_equal(
    \{'regcontents': ['bar'], 'regname': 'b', 'operator': 'd', 'regtype': 'v'},
    \g:event)

  call assert_equal({}, v:event)

  au! TextYankPost
  unlet g:event
  bwipe!
endfunc

func Test_nocatch_wipe_all_buffers()
  " Real nasty autocommand: wipe all buffers on any event.
  au * * bwipe *
  " Get E93 first?
  " call assert_fails('next x', 'E93:')
  call assert_fails('next x', 'E517:')
  bwipe
  au!
endfunc

func Test_nocatch_wipe_dummy_buffer()
  if has('quickfix')
    " Nasty autocommand: wipe buffer on any event.
    au * x bwipe
    call assert_fails('lv½ /x', 'E480')
    au!
  endif
endfunc

function s:Before_test_dirchanged()
  augroup test_dirchanged
    autocmd!
  augroup END
  let s:li = []
  let s:dir_this = getcwd()
  let s:dir_foo = s:dir_this . '/foo'
  call mkdir(s:dir_foo)
  let s:dir_bar = s:dir_this . '/bar'
  call mkdir(s:dir_bar)
endfunc

function s:After_test_dirchanged()
  call chdir(s:dir_this)
  call delete(s:dir_foo, 'd')
  call delete(s:dir_bar, 'd')
  augroup test_dirchanged
    autocmd!
  augroup END
endfunc

function Test_dirchanged_global()
  call s:Before_test_dirchanged()
  autocmd test_dirchanged DirChanged global call add(s:li, "cd:")
  autocmd test_dirchanged DirChanged global call add(s:li, expand("<afile>"))
  call chdir(s:dir_foo)
  call assert_equal(["cd:", s:dir_foo], s:li)
  call chdir(s:dir_foo)
  call assert_equal(["cd:", s:dir_foo], s:li)
  exe 'lcd ' .. fnameescape(s:dir_bar)
  call assert_equal(["cd:", s:dir_foo], s:li)
  call s:After_test_dirchanged()
endfunc

function Test_dirchanged_local()
  call s:Before_test_dirchanged()
  autocmd test_dirchanged DirChanged window call add(s:li, "lcd:")
  autocmd test_dirchanged DirChanged window call add(s:li, expand("<afile>"))
  call chdir(s:dir_foo)
  call assert_equal([], s:li)
  exe 'lcd ' .. fnameescape(s:dir_bar)
  call assert_equal(["lcd:", s:dir_bar], s:li)
  exe 'lcd ' .. fnameescape(s:dir_bar)
  call assert_equal(["lcd:", s:dir_bar], s:li)
  call s:After_test_dirchanged()
endfunc

function Test_dirchanged_auto()
  if !exists('+autochdir')
    return
  endif
  call s:Before_test_dirchanged()
  call test_autochdir()
  autocmd test_dirchanged DirChanged auto call add(s:li, "auto:")
  autocmd test_dirchanged DirChanged auto call add(s:li, expand("<afile>"))
  set acd
  cd ..
  call assert_equal([], s:li)
  exe 'edit ' . s:dir_foo . '/Xfile'
  call assert_equal(s:dir_foo, getcwd())
  call assert_equal(["auto:", s:dir_foo], s:li)
  set noacd
  bwipe!
  call s:After_test_dirchanged()
endfunc

" Test TextChangedI and TextChangedP
func Test_ChangedP()
  new
  call setline(1, ['foo', 'bar', 'foobar'])
  call test_override("char_avail", 1)
  set complete=. completeopt=menuone

  func! TextChangedAutocmd(char)
    let g:autocmd .= a:char
  endfunc

  au! TextChanged <buffer> :call TextChangedAutocmd('N')
  au! TextChangedI <buffer> :call TextChangedAutocmd('I')
  au! TextChangedP <buffer> :call TextChangedAutocmd('P')

  call cursor(3, 1)
  let g:autocmd = ''
  call feedkeys("o\<esc>", 'tnix')
  call assert_equal('I', g:autocmd)

  let g:autocmd = ''
  call feedkeys("Sf", 'tnix')
  call assert_equal('II', g:autocmd)

  let g:autocmd = ''
  call feedkeys("Sf\<C-N>", 'tnix')
  call assert_equal('IIP', g:autocmd)

  let g:autocmd = ''
  call feedkeys("Sf\<C-N>\<C-N>", 'tnix')
  call assert_equal('IIPP', g:autocmd)

  let g:autocmd = ''
  call feedkeys("Sf\<C-N>\<C-N>\<C-N>", 'tnix')
  call assert_equal('IIPPP', g:autocmd)

  let g:autocmd = ''
  call feedkeys("Sf\<C-N>\<C-N>\<C-N>\<C-N>", 'tnix')
  call assert_equal('IIPPPP', g:autocmd)

  call assert_equal(['foo', 'bar', 'foobar', 'foo'], getline(1, '$'))
  " TODO: how should it handle completeopt=noinsert,noselect?

  " CleanUp
  call test_override("char_avail", 0)
  au! TextChanged
  au! TextChangedI
  au! TextChangedP
  delfu TextChangedAutocmd
  unlet! g:autocmd
  set complete&vim completeopt&vim

  bw!
endfunc

let g:setline_handled = v:false
func SetLineOne()
  if !g:setline_handled
    call setline(1, "(x)")
    let g:setline_handled = v:true
  endif
endfunc

func Test_TextChangedI_with_setline()
  new
  call test_override('char_avail', 1)
  autocmd TextChangedI <buffer> call SetLineOne()
  call feedkeys("i(\<CR>\<Esc>", 'tx')
  call assert_equal('(', getline(1))
  call assert_equal('x)', getline(2))
  undo
  call assert_equal('', getline(1))
  call assert_equal('', getline(2))

  call test_override('starting', 0)
  bwipe!
endfunc

func Test_Changed_FirstTime()
  CheckFeature terminal
  CheckNotGui

  " Prepare file for TextChanged event.
  call writefile([''], 'Xchanged.txt')
  let buf = term_start([GetVimProg(), '--clean', '-c', 'set noswapfile'], {'term_rows': 3})
  call assert_equal('running', term_getstatus(buf))
  " Wait for the ruler (in the status line) to be shown.
  " In ConPTY, there is additional character which is drawn up to the width of
  " the screen.
  if has('conpty')
    call WaitForAssert({-> assert_match('\<All.*$', term_getline(buf, 3))})
  else
    call WaitForAssert({-> assert_match('\<All$', term_getline(buf, 3))})
  endif
  " It's only adding autocmd, so that no event occurs.
  call term_sendkeys(buf, ":au! TextChanged <buffer> call writefile(['No'], 'Xchanged.txt')\<cr>")
  call term_sendkeys(buf, "\<C-\\>\<C-N>:qa!\<cr>")
  call WaitForAssert({-> assert_equal('finished', term_getstatus(buf))})
  call assert_equal([''], readfile('Xchanged.txt'))

  " clean up
  call delete('Xchanged.txt')
  bwipe!
endfunc

func Test_autocmd_nested()
  let g:did_nested = 0
  augroup Testing
    au WinNew * edit somefile
    au BufNew * let g:did_nested = 1
  augroup END
  split
  call assert_equal(0, g:did_nested)
  close
  bwipe! somefile

  " old nested argument still works
  augroup Testing
    au!
    au WinNew * nested edit somefile
    au BufNew * let g:did_nested = 1
  augroup END
  split
  call assert_equal(1, g:did_nested)
  close
  bwipe! somefile

  " New ++nested argument works
  augroup Testing
    au!
    au WinNew * ++nested edit somefile
    au BufNew * let g:did_nested = 1
  augroup END
  split
  call assert_equal(1, g:did_nested)
  close
  bwipe! somefile

  augroup Testing
    au!
  augroup END

  call assert_fails('au WinNew * ++nested ++nested echo bad', 'E983:')
  call assert_fails('au WinNew * nested nested echo bad', 'E983:')
endfunc

func Test_autocmd_once()
  " Without ++once WinNew triggers twice
  let g:did_split = 0
  augroup Testing
    au WinNew * let g:did_split += 1
  augroup END
  split
  split
  call assert_equal(2, g:did_split)
  call assert_true(exists('#WinNew'))
  close
  close

  " With ++once WinNew triggers once
  let g:did_split = 0
  augroup Testing
    au!
    au WinNew * ++once let g:did_split += 1
  augroup END
  split
  split
  call assert_equal(1, g:did_split)
  call assert_false(exists('#WinNew'))
  close
  close

  call assert_fails('au WinNew * ++once ++once echo bad', 'E983:')
endfunc

func Test_autocmd_bufreadpre()
  new
  let b:bufreadpre = 1
  call append(0, range(100))
  w! XAutocmdBufReadPre.txt
  autocmd BufReadPre <buffer> :let b:bufreadpre += 1
  norm! 50gg
  sp
  norm! 100gg
  wincmd p
  let g:wsv1 = winsaveview()
  wincmd p
  let g:wsv2 = winsaveview()
  " triggers BufReadPre, should not move the cursor in either window
  " The topline may change one line in a large window.
  edit
  call assert_inrange(g:wsv2.topline - 1, g:wsv2.topline + 1, winsaveview().topline)
  call assert_equal(g:wsv2.lnum, winsaveview().lnum)
  call assert_equal(2, b:bufreadpre)
  wincmd p
  call assert_equal(g:wsv1.topline, winsaveview().topline)
  call assert_equal(g:wsv1.lnum, winsaveview().lnum)
  call assert_equal(2, b:bufreadpre)
  " Now set the cursor position in an BufReadPre autocommand
  " (even though the position will be invalid, this should make Vim reset the
  " cursor position in the other window.
  wincmd p
  set cpo+=g
  " won't do anything, but try to set the cursor on an invalid lnum
  autocmd BufReadPre <buffer> :norm! 70gg
  " triggers BufReadPre, should not move the cursor in either window
  e
  call assert_equal(1, winsaveview().topline)
  call assert_equal(1, winsaveview().lnum)
  call assert_equal(3, b:bufreadpre)
  wincmd p
  call assert_equal(g:wsv1.topline, winsaveview().topline)
  call assert_equal(g:wsv1.lnum, winsaveview().lnum)
  call assert_equal(3, b:bufreadpre)
  close
  close
  call delete('XAutocmdBufReadPre.txt')
  set cpo-=g
endfunc

" FileChangedShell tested in test_filechanged.vim

" Tests for the following autocommands:
" - FileWritePre	writing a compressed file
" - FileReadPost	reading a compressed file
" - BufNewFile		reading a file template
" - BufReadPre		decompressing the file to be read
" - FilterReadPre	substituting characters in the temp file
" - FilterReadPost	substituting characters after filtering
" - FileReadPre		set options for decompression
" - FileReadPost	decompress the file
func Test_ReadWrite_Autocmds()
  " Run this test only on Unix-like systems and if gzip is available
  if !has('unix') || !executable("gzip")
    return
  endif

  " Make $GZIP empty, "-v" would cause trouble.
  let $GZIP = ""

  " Use a FileChangedShell autocommand to avoid a prompt for 'Xtestfile.gz'
  " being modified outside of Vim (noticed on Solaris).
  au FileChangedShell * echo 'caught FileChangedShell'

  " Test for the FileReadPost, FileWritePre and FileWritePost autocmds
  augroup Test1
    au!
    au FileWritePre    *.gz   '[,']!gzip
    au FileWritePost   *.gz   undo
    au FileReadPost    *.gz   '[,']!gzip -d
  augroup END

  new
  set bin
  call append(0, [
	      \ 'line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ])
  1,9write! Xtestfile.gz
  enew! | close

  new
  " Read and decompress the testfile
  0read Xtestfile.gz
  call assert_equal([
	      \ 'line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ], getline(1, 9))
  enew! | close

  augroup Test1
    au!
  augroup END

  " Test for the FileAppendPre and FileAppendPost autocmds
  augroup Test2
    au!
    au BufNewFile      *.c    read Xtest.c
    au FileAppendPre   *.out  '[,']s/new/NEW/
    au FileAppendPost  *.out  !cat Xtest.c >> test.out
  augroup END

  call writefile(['/*', ' * Here is a new .c file', ' */'], 'Xtest.c')
  new foo.c			" should load Xtest.c
  call assert_equal(['/*', ' * Here is a new .c file', ' */'], getline(2, 4))
  w! >> test.out		" append it to the output file

  let contents = readfile('test.out')
  call assert_equal(' * Here is a NEW .c file', contents[2])
  call assert_equal(' * Here is a new .c file', contents[5])

  call delete('test.out')
  enew! | close
  augroup Test2
    au!
  augroup END

  " Test for the BufReadPre and BufReadPost autocmds
  augroup Test3
    au!
    " setup autocommands to decompress before reading and re-compress
    " afterwards
    au BufReadPre  *.gz  exe '!gzip -d ' . shellescape(expand("<afile>"))
    au BufReadPre  *.gz  call rename(expand("<afile>:r"), expand("<afile>"))
    au BufReadPost *.gz  call rename(expand("<afile>"), expand("<afile>:r"))
    au BufReadPost *.gz  exe '!gzip ' . shellescape(expand("<afile>:r"))
  augroup END

  e! Xtestfile.gz		" Edit compressed file
  call assert_equal([
	      \ 'line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ], getline(1, 9))

  w! >> test.out		" Append it to the output file

  augroup Test3
    au!
  augroup END

  " Test for the FilterReadPre and FilterReadPost autocmds.
  set shelltemp			" need temp files here
  augroup Test4
    au!
    au FilterReadPre   *.out  call rename(expand("<afile>"), expand("<afile>") . ".t")
    au FilterReadPre   *.out  exe 'silent !sed s/e/E/ ' . shellescape(expand("<afile>")) . ".t >" . shellescape(expand("<afile>"))
    au FilterReadPre   *.out  exe 'silent !rm ' . shellescape(expand("<afile>")) . '.t'
    au FilterReadPost  *.out  '[,']s/x/X/g
  augroup END

  e! test.out			" Edit the output file
  1,$!cat
  call assert_equal([
	      \ 'linE 2	AbcdefghijklmnopqrstuvwXyz',
	      \ 'linE 3	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX',
	      \ 'linE 4	AbcdefghijklmnopqrstuvwXyz',
	      \ 'linE 5	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX',
	      \ 'linE 6	AbcdefghijklmnopqrstuvwXyz',
	      \ 'linE 7	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX',
	      \ 'linE 8	AbcdefghijklmnopqrstuvwXyz',
	      \ 'linE 9	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX',
	      \ 'linE 10 AbcdefghijklmnopqrstuvwXyz'
	      \ ], getline(1, 9))
  call assert_equal([
	      \ 'line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ], readfile('test.out'))

  augroup Test4
    au!
  augroup END
  set shelltemp&vim

  " Test for the FileReadPre and FileReadPost autocmds.
  augroup Test5
    au!
    au FileReadPre *.gz exe 'silent !gzip -d ' . shellescape(expand("<afile>"))
    au FileReadPre *.gz call rename(expand("<afile>:r"), expand("<afile>"))
    au FileReadPost *.gz '[,']s/l/L/
  augroup END

  new
  0r Xtestfile.gz		" Read compressed file
  call assert_equal([
	      \ 'Line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'Line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'Line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'Line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'Line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'Line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'Line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'Line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'Line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ], getline(1, 9))
  call assert_equal([
	      \ 'line 2	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 3	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 4	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 5	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 6	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 7	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 8	Abcdefghijklmnopqrstuvwxyz',
	      \ 'line 9	xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',
	      \ 'line 10 Abcdefghijklmnopqrstuvwxyz'
	      \ ], readfile('Xtestfile.gz'))

  augroup Test5
    au!
  augroup END

  au! FileChangedShell
  call delete('Xtestfile.gz')
  call delete('Xtest.c')
  call delete('test.out')
endfunc

func Test_throw_in_BufWritePre()
  new
  call setline(1, ['one', 'two', 'three'])
  call assert_false(filereadable('Xthefile'))
  augroup throwing
    au BufWritePre X* throw 'do not write'
  augroup END
  try
    w Xthefile
  catch
    let caught = 1
  endtry
  call assert_equal(1, caught)
  call assert_false(filereadable('Xthefile'))

  bwipe!
  au! throwing
endfunc

func Test_autocmd_SafeState()
  CheckRunVimInTerminal

  let lines =<< trim END
	let g:safe = 0
	let g:again = ''
	au SafeState * let g:safe += 1
	au SafeStateAgain * let g:again ..= 'x'
	func CallTimer()
	  call timer_start(10, {id -> execute('let g:again ..= "t"')})
	endfunc
  END
  call writefile(lines, 'XSafeState')
  let buf = RunVimInTerminal('-S XSafeState', #{rows: 6})

  " Sometimes we loop to handle an K_IGNORE
  call term_sendkeys(buf, ":echo g:safe\<CR>")
  call WaitForAssert({-> assert_match('^[12] ', term_getline(buf, 6))}, 1000)

  call term_sendkeys(buf, ":echo g:again\<CR>")
  call WaitForAssert({-> assert_match('^xxxx', term_getline(buf, 6))}, 1000)

  call term_sendkeys(buf, ":let g:again = ''\<CR>:call CallTimer()\<CR>")
  call term_wait(buf)
  call term_sendkeys(buf, ":\<CR>")
  call term_wait(buf)
  call term_sendkeys(buf, ":echo g:again\<CR>")
  call WaitForAssert({-> assert_match('xtx', term_getline(buf, 6))}, 1000)

  call StopVimInTerminal(buf)
  call delete('XSafeState')
endfunc

func Test_autocmd_CmdWinEnter()
  CheckRunVimInTerminal
  " There is not cmdwin switch, so
  " test for cmdline_hist
  " (both are available with small builds)
  CheckFeature cmdline_hist
  let lines =<< trim END
    let b:dummy_var = 'This is a dummy'
    autocmd CmdWinEnter * quit
    let winnr = winnr('$')
  END
  let filename='XCmdWinEnter'
  call writefile(lines, filename)
  let buf = RunVimInTerminal('-S '.filename, #{rows: 6})

  call term_sendkeys(buf, "q:")
  call term_wait(buf)
  call term_sendkeys(buf, ":echo b:dummy_var\<cr>")
  call WaitForAssert({-> assert_match('^This is a dummy', term_getline(buf, 6))}, 1000)
  call term_sendkeys(buf, ":echo &buftype\<cr>")
  call WaitForAssert({-> assert_notmatch('^nofile', term_getline(buf, 6))}, 1000)
  call term_sendkeys(buf, ":echo winnr\<cr>")
  call WaitForAssert({-> assert_match('^1', term_getline(buf, 6))}, 1000)

  " clean up
  call StopVimInTerminal(buf)
  call delete(filename)
endfunc

func Test_autocmd_was_using_freed_memory()
  CheckFeature quickfix

  pedit xx
  n x
  au WinEnter * quit
  split
  au! WinEnter
endfunc

func Test_BufWrite_lockmarks()
  edit! Xtest
  call setline(1, ['a', 'b', 'c', 'd'])

  " :lockmarks preserves the marks
  call SetChangeMarks(2, 3)
  lockmarks write
  call assert_equal([2, 3], [line("'["), line("']")])

  " *WritePre autocmds get the correct line range, but lockmarks preserves the
  " original values for the user
  augroup lockmarks
    au!
    au BufWritePre,FilterWritePre * call assert_equal([1, 4], [line("'["), line("']")])
    au FileWritePre * call assert_equal([3, 4], [line("'["), line("']")])
  augroup END

  lockmarks write
  call assert_equal([2, 3], [line("'["), line("']")])

  if executable('cat')
    lockmarks %!cat
    call assert_equal([2, 3], [line("'["), line("']")])
  endif

  lockmarks 3,4write Xtest2
  call assert_equal([2, 3], [line("'["), line("']")])

  au! lockmarks
  augroup! lockmarks
  call delete('Xtest')
  call delete('Xtest2')
endfunc
