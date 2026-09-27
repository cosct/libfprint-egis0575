# 研究仓库评测小工具。依赖仓库内 libfprint/ 子树（相对本目录为
# ../libfprint/，见 README 构建步骤）：
#   make                # egis0575-matcher-test（C matcher 交叉验证）
#                       # + egis0575-feedback-eval（验证回馈 FAR 离线复核）
#   make eval_bz3       # 离线 Bozorth3 评测（先构建 libfprint 得到 NBIS 静态库：
#                       #   meson setup ../libfprint/builddir ../libfprint && \
#                       #   meson compile -C ../libfprint/builddir）
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
DRV     := ../libfprint/libfprint/drivers
NBISINC := ../libfprint/libfprint/nbis/include \
           ../libfprint/libfprint/nbis/libfprint-include \
           ../libfprint/libfprint
NBISLIB := ../libfprint/builddir/libfprint/libnbis.a
GLIBCF  := $(shell pkg-config --cflags glib-2.0)
GLIBLD  := $(shell pkg-config --libs glib-2.0)

.PHONY: all clean

all: egis0575-matcher-test egis0575-feedback-eval

egis0575-matcher-test: egis0575-matcher-test.c \
                       $(DRV)/egis0575-matcher.c $(DRV)/egis0575-matcher.h
	$(CC) $(CFLAGS) -I$(DRV) -o $@ egis0575-matcher-test.c \
	      $(DRV)/egis0575-matcher.c -lm

egis0575-feedback-eval: egis0575-feedback-eval.c \
                        $(DRV)/egis0575-matcher.c $(DRV)/egis0575-matcher.h
	$(CC) $(CFLAGS) -I$(DRV) $(GLIBCF) -o $@ egis0575-feedback-eval.c \
	      $(DRV)/egis0575-matcher.c $(GLIBLD) -lm

$(NBISLIB):
	@echo "缺少 $(NBISLIB)——先构建 libfprint（见文件头注释）" >&2; exit 1

eval_bz3: eval_bz3.c $(NBISLIB)
	$(CC) $(CFLAGS) $(addprefix -I,$(NBISINC)) $(GLIBCF) -o $@ eval_bz3.c \
	      $(NBISLIB) $(GLIBLD) -lm

clean:
	rm -f egis0575-matcher-test egis0575-feedback-eval eval_bz3
