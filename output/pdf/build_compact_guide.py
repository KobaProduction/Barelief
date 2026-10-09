from pathlib import Path
from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import A4
from reportlab.lib.colors import HexColor, white
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import Paragraph, Table, TableStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

ROOT=Path(__file__).resolve().parent
pdfmetrics.registerFont(TTFont('A','C:/Windows/Fonts/arial.ttf'))
pdfmetrics.registerFont(TTFont('AB','C:/Windows/Fonts/arialbd.ttf'))
pdfmetrics.registerFontFamily('A',normal='A',bold='AB',italic='A',boldItalic='AB')
W,H=A4; M=25; GAP=19; CW=(W-2*M-GAP)/2
INK=HexColor('#15333D'); TEAL=HexColor('#176C77'); PALE=HexColor('#EFF5F5'); LINE=HexColor('#D4E0E1'); GRAY=HexColor('#52646B'); ORANGE=HexColor('#B75D29')
S={
 'p':ParagraphStyle('p',fontName='A',fontSize=10,leading=13.4,textColor=INK),
 's':ParagraphStyle('s',fontName='A',fontSize=8.5,leading=11.2,textColor=GRAY),
 'h':ParagraphStyle('h',fontName='AB',fontSize=13,leading=16,textColor=TEAL),
 'box':ParagraphStyle('box',fontName='A',fontSize=9.4,leading=12.4,textColor=INK),
}
c=canvas.Canvas(str(ROOT/'codex-workflows-ru.pdf'),pagesize=A4)
c.setTitle('Codex: компактная памятка на 2 страницы');c.setAuthor('Codex')

def para(text,x,y,width=CW,style='p',after=7):
 p=Paragraph(text,S[style]);pw,ph=p.wrap(width,H);p.drawOn(c,x,y-ph);return y-ph-after
def section(number,title,x,y):
 c.setFillColor(ORANGE);c.roundRect(x,y-19,23,20,4,fill=1,stroke=0);c.setFont('AB',10);c.setFillColor(white);c.drawCentredString(x+11.5,y-12.5,number)
 y=para(title,x+31,y,CW-31,'h',0);return y-10
def box(text,x,y):
 p=Paragraph(text,S['box']);_,ph=p.wrap(CW-20,H);bh=ph+18
 c.setFillColor(PALE);c.roundRect(x,y-bh,CW,bh,5,fill=1,stroke=0);c.setFillColor(TEAL);c.rect(x,y-bh+5,2,bh-10,fill=1,stroke=0);p.drawOn(c,x+10,y-9-ph);return y-bh-10
def commands(rows,x,y):
 cells=[[Paragraph('<b>'+a+'</b>',S['box']),Paragraph(b,S['box'])] for a,b in rows]
 t=Table(cells,colWidths=[73,CW-73]);t.setStyle(TableStyle([('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),4),('RIGHTPADDING',(0,0),(-1,-1),4),('TOPPADDING',(0,0),(-1,-1),5),('BOTTOMPADDING',(0,0),(-1,-1),5),('LINEBELOW',(0,0),(-1,-1),0.4,LINE)]));_,th=t.wrap(CW,H);t.drawOn(c,x,y-th);return y-th-10
def header(page,subtitle):
 c.setFillColor(TEAL);c.rect(0,H-7,W,7,fill=1,stroke=0)
 c.setFont('AB',25);c.setFillColor(INK);c.drawString(M,H-43,'CODEX')
 c.setFont('A',11);c.setFillColor(GRAY);c.drawString(M+103,H-42,'Памятка по рабочим процессам')
 c.setFont('A',9);c.drawString(M,H-60,subtitle)
 c.setFont('AB',10);c.setFillColor(TEAL);c.drawRightString(W-M,H-43,f'{page} / 2')
 c.setStrokeColor(LINE);c.line(M,H-72,W-M,H-72)
 return H-88
def footer(page,text):
 c.setStrokeColor(LINE);c.line(M,42,W-M,42);para(text,M,33,W-2*M,'s',0)
 c.setFont('A',8);c.setFillColor(GRAY);c.drawRightString(W-M,12,f'04.10.2026  •  {page}/2')

x1=M;x2=M+CW+GAP
y=header(1,'План → цель → делегирование → проверка → результат')
l=section('01','План и цель',x1,y)
l=para('<b>План</b> задаёт шаги и границы. <b>Цель</b> определяет, что считать готовым. <b>Чеклист</b> фиксирует прогресс.',x1,l)
l=commands([('/plan','Изучить задачу и предложить подход до правок.'),('/goal','Продолжать работу к постоянной цели в этом чате.'),('/review','Проверить изменения и риски.'),('/status','Посмотреть контекст и лимиты.'),('/compact','Сжать длинный разговор.'),('/init','Создать основу AGENTS.md.')],x1,l)
l=box('<b>Хорошее задание содержит:</b><br/>1. Нужный результат.<br/>2. Проверку готовности.<br/>3. Ограничения и разрешённую область.<br/>4. Условие остановки или блокера.',x1,l)
l=section('02','Рабочий запрос',x1,l-6)
l=box('Сначала предложи план. После согласования выполни его по этапам. Веди чеклист в PLAN.md. Заверши, когда требования выполнены и нужные проверки проходят. Сообщи изменения, проверки и нерешённые вопросы.',x1,l)
l=para('<b>Для долгой работы:</b> фиксируй решения и состояние в коротких файлах. Сжатие переписки не гарантирует сохранения всех подробностей.',x1,l)
l=para('Команды относятся к десктопному Codex. Доступный набор смотри через /. В веб-версии меню отличается.',x1,l,style='s')

r=section('03','Субагенты',x2,y)
r=para('Делегируй <b>независимые части</b>: исследование, проверку рисков, изменения отдельных модулей. Каждому назначай конкретный вопрос, файлы и результат.',x2,r)
r=box('Запусти до 3 субагентов параллельно. Раздели ответственность. Не откатывайте чужие правки. Итог каждого: вывод, доказательство, изменения, проверки и блокеры.',x2,r)
r=para('<b>Управление через координатора:</b><br/>• Покажи состояние агентов.<br/>• Передай второму уточнение…<br/>• Останови первого.<br/>• Дай третьему следующую задачу.<br/>• Дождись всех и собери результаты.',x2,r)
r=section('04','Экономия контекста',x2,r-5)
r=commands([('none','fork_turns="none": без истории родителя.'),('all','fork_turns="all": вся доступная история на момент запуска.'),('N','fork_turns="3": последние 3 хода разговора.')],x2,r)
r=para('Это параметры инструмента <b>этого сеанса</b>, а не команды интерфейса. Новые сведения передавай сообщением или через файлы. Уже сжатую историю all не восстановит.',x2,r,style='s')
r=box('<b>Короткий отчёт, до 200 слов:</b><br/>статус → вывод → подтверждение → файлы → проверки → блокеры.<br/>Логи и подробности сохранить отдельно; координатору дать путь.',x2,r)
r=para('<b>Не начинать заново:</b> агент сохраняет, что сделано, что проверено и следующий шаг. Связанные задания отдавай существующему агенту.',x2,r)
r=para('Предел отчёта - инструкция, не технический лимит. Субагенты тоже расходуют токены. В этом сеансе: координатор + до 3 агентов.',x2,r,style='s')
assert min(l,r)>55,(l,r)
footer(1,'Самое полезное: короткое задание + проверяемая цель + нужные файлы + компактный отчёт.')
c.showPage()

y=header(2,'Параллельные чаты, постоянные инструкции и автоматические проверки')
l=section('05','Чаты и worktrees',x1,y)
l=commands([('Новый чат','Самодостаточное задание; история соседнего чата не копируется.'),('/fork','Новый чат с копией исходного контекста.'),('/side','Боковой разговор для сопутствующего вопроса.'),('/worktree','Отдельная рабочая копия Git для параллельных изменений.')],x1,l)
l=para('<b>Одна общая задача:</b> субагенты.<br/><b>Разные самостоятельные задачи:</b> отдельные чаты, при правках - worktrees.',x1,l)
l=box('Создай два чата в worktrees для независимых задач. Координируй их здесь. Передай краткие задания и критерии готовности. Читай компактные статусы, подробности - при блокере и перед интеграцией.',x1,l)
l=para('Worktree изолирует файлы, но не задаёт историю разговора. Итоговые изменения нужно совместить и проверить. Для передачи поручений другим чатам дай координатору прямое указание.',x1,l,style='s')
l=section('06','Постоянный контекст',x1,l-5)
l=para('<b>AGENTS.md</b> - правила проекта, команды проверок и ограничения.<br/><b>Skill</b> - повторяемый процесс с инструкциями и ресурсами.<br/><b>Запись состояния</b> - цель, решения, выполненное, проверки и следующий шаг.',x1,l)
l=box('<b>Правило экономии:</b><br/>Передавай короткое актуальное состояние и ссылки на доказательства. Не заставляй каждого агента читать весь журнал работы.',x1,l)
l=para('Общая папка даёт доступ к файлам, а не общую память переписки. Для worktrees проверь, что нужные документы доступны в каждой копии.',x1,l,style='s')

r=section('07','Веб и автоматизации',x2,y)
r=para('<b>ChatGPT Work в вебе</b> может планировать многошаговую работу. Десктопные /plan и /goal автоматически на веб не переносятся.',x2,r)
r=commands([('Расписание','Регулярная проверка в заданное время.'),('В этом чате','Возврат с существующим контекстом.'),('Отдельно','Независимый запуск по сохранённому заданию.'),('Событие','Поддерживаемая активность GitHub PR, письма Gmail, сообщения Slack.')],x2,r)
r=box('Раз в час проверяй указанный PR в этом чате. Сообщай только о новых замечаниях, блокерах или завершении. После слияния прекрати проверки.',x2,r)
r=para('<b>При настройке укажи:</b> источник, условие запуска, действие, критерий уведомления и остановку. Проверь сохранённую задачу и историю запусков в <b>Scheduled</b>.',x2,r)
r=para('Событийные триггеры доступны в вебе и на мобильных устройствах на подходящих тарифах. В одной задаче нельзя смешивать события с расписанием. Для локальных проверок в десктопе нужны включённый компьютер и приложение.',x2,r,style='s')
r=section('08','Браузерная проверка',x2,r-5)
r=para('<b>@Browser в десктопе:</b> открыть страницу, повторить действия и проверить результат. <b>Annotate:</b> выделить элемент, оставить замечание и попросить исправление. Для существующих вкладок есть расширение браузера.',x2,r)
assert min(l,r)>135,(l,r)

c.setStrokeColor(LINE);c.line(M,127,W-M,127)
sy=117
sy=para('<b>Источники OpenAI: learn.chatgpt.com</b> • Доступность зависит от версии и тарифа.',M,sy,W-2*M,'s',4)
links=[('Команды','reference/slash-commands'),('Субагенты','agent-configuration/subagents'),('Worktrees','environments/git-worktrees'),('Автоматизации','automations'),('Браузер','browser')]
for i in range(0,len(links),2):
 for j,(label,path) in enumerate(links[i:i+2]):
  para(f'{label}: <link href="https://learn.chatgpt.com/docs/{path}" color="#176C77">docs/{path}</link>',[x1,x2][j],sy,CW,'s',0)
 sy-=22
footer(2,'Параметры fork_turns и лимит агентов приведены для текущего сеанса; это не универсальные настройки.')
c.save()
print(ROOT/'codex-workflows-ru.pdf')
