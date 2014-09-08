# IBM C/C++ (VisualAge) Makefile for CPMAP
#

CC     = icc.exe
RC     = rc.exe
LINK   = ilink.exe
CFLAGS = /Gm /Ss /Q /Wuse
RFLAGS = -n
LFLAGS = /PMTYPE:PM /NOLOGO /MAP
NAME   = cpmap
OBJS   = $(NAME).obj
MRI    = $(NAME)
LIBS   = libconv.lib libuls.lib

!ifdef DEBUG
    CFLAGS   = $(CFLAGS) /Ti /Tm
    LFLAGS   = $(LFLAGS) /DEBUG
!endif


$(NAME).exe : $(OBJS) $(NAME).res ids.h
                $(LINK) $(LFLAGS) $(OBJS) $(LIBS)
                $(RC) $(RFLAGS) $(MRI).res $@

$(NAME).res : $(NAME).rc $(NAME).dlg $(NAME).ico ids.h
                $(RC) -r $(NAME).rc $@

clean       :
              -del $(OBJS)$(NAME).res $(NAME).exe $(NAME).map 2>NUL


