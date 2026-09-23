" Vim syntax file
" Language: Tiq
" Maintainer: Tiq Language Team
" Latest Revision: 22 September 2026

if exists("b:current_syntax")
  finish
endif

" Keywords
syn keyword tiqKeyword struct enum match
syn keyword tiqKeyword skip break
syn keyword tiqConstant true false none some ok err

" Types
syn keyword tiqType i64 i32 f64 bool str void

" Operators
syn match tiqOperator "->"
syn match tiqOperator "??"
syn match tiqOperator "?"
syn match tiqOperator ":"
syn match tiqOperator "="
syn match tiqOperator "<-"
syn match tiqOperator "+"
syn match tiqOperator "-"
syn match tiqOperator "\*"
syn match tiqOperator "/"
syn match tiqOperator "%"
syn match tiqOperator "=="
syn match tiqOperator "!="
syn match tiqOperator "<="
syn match tiqOperator ">="
syn match tiqOperator "<"
syn match tiqOperator ">"
syn match tiqOperator "&&"
syn match tiqOperator "||"
syn match tiqOperator "!"
syn match tiqOperator "&"
syn match tiqOperator "|"
syn match tiqOperator "\^"
syn match tiqOperator "\~"
syn match tiqOperator "<<"
syn match tiqOperator ">>"

" Numbers
syn match tiqNumber "\<\d\+\>"

" Strings
syn region tiqString start=/"/ skip=/\\"/ end=/"/
syn match tiqEscape "\\[nrt0\\\"]" contained
syn region tiqString start=/"/ skip=/\\"/ end=/"/ contains=tiqEscape

" Comments
syn region tiqComment start=/\/\// end=/$/

" Function definitions
syn match tiqFunction "\<[a-zA-Z_][a-zA-Z0-9_]*\>" nextgroup=tiqParams

" Brackets and braces
syn match tiqBracket "[\[\](){}]"

" Highlighting
hi def link tiqKeyword Keyword
hi def link tiqType Type
hi def link tiqOperator Operator
hi def link tiqNumber Number
hi def link tiqString String
hi def link tiqEscape Special
hi def link tiqComment Comment
hi def link tiqFunction Function
hi def link tiqConstant Constant
hi def link tiqBracket Delimiter

let b:current_syntax = "tiq"
