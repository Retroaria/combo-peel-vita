# ComboPeel (PS Vita)

Plugin de kernel para PS Vita que desbloqueia a tela de bloqueio com um combo de botões,
com a mesma animação de "descascar" (peel) do gesto com o dedo.

**Combo:** analógico esquerdo para baixo + X

## Como usar

1. Acorde o Vita pelo botão **PS** ou pelo **Power**. Aparece a tela de bloqueio com o relógio.
2. Incline o **analógico esquerdo para baixo** e aperte **X**.
3. O plugin arrasta um "dedo virtual" do canto superior direito até o centro, e a tela é desbloqueada.

Desbloquear com o dedo continua funcionando normalmente.

### Proteções contra acionamento acidental

- O combo só vale nos **20 segundos** depois que o Vita acorda ou que o botão Power é apertado,
  e nos **2 primeiros minutos** depois de ligar.
- Vale **uma vez** por janela.
- É preciso apertar o combo (não adianta já estar segurando quando a tela acende).
- Apertar o botão PS com o Vita já acordado (por exemplo, dentro de um jogo) **não** arma o combo,
  porque no LiveArea o mesmo gesto fecharia o jogo.

## Instalação (recomendado: pelo VPK)

Requisitos: HENkaku/Ensō com **"Enable Unsafe Homebrew"** ativado nas HENkaku Settings.

1. Baixe o `ComboPeel.vpk` na página de **Releases**.
2. Copie para o cartão e instale pelo VitaShell (arquivo › Install).
3. Abra o app **ComboPeel** e aperte **X (Instalar / atualizar)**. Ele:
   - copia o plugin para `ur0:tai/combo_peel.skprx`;
   - faz backup do `config.txt` com data e hora no nome
     (ex.: `config_2026-11-16_13-14_antes-de-instalar-combopeel.bak`);
   - adiciona a linha na seção `*KERNEL`, sem duplicar.
4. Aperte **SELECT** para reiniciar.
5. Depois pode deletar o app (Triângulo › Delete na bolha): o plugin continua instalado.

**Para remover o plugin:** instale o VPK de novo, abra e aperte **Triângulo (Desinstalar)**.
Isso tira só a linha do ComboPeel do `config.txt` atual e mantém todo o resto.
Os backups nunca são restaurados automaticamente; servem só para consulta.

### Instalação manual

1. Copie `combo_peel.skprx` para `ur0:tai/` (pelo VitaShell).
2. No `ur0:tai/config.txt`, adicione `ur0:tai/combo_peel.skprx` na seção `*KERNEL`.
3. Reinicie.

## Registro (log)

O plugin grava um registro curto em `ux0:data/combopeel.log`: quando o Vita acorda, quando o combo
é armado e quando o gesto é disparado. Ele só grava quando algo acontece e se limpa sozinho
se passar de 256 KB. Se algo não funcionar, anexe esse arquivo ao reportar o problema.

## Se algo der errado

- Se o Vita travar na inicialização, segure **L** enquanto liga para pular os plugins
  e remova a linha do `config.txt`.

## Compilar

O GitHub Actions compila a cada envio e publica os arquivos nos releases (tags `v*`).
Para compilar localmente com o VitaSDK:

```
cmake -S . -B build
cmake --build build
```

## Como funciona

- Intercepta as funções de leitura do touch no kernel (`ksceTouchPeek/Read` e variantes `Region`)
  e, durante ~0,6 s, substitui os dados pelo gesto de um dedo virtual.
- Lê os botões e o analógico com `ksceCtrlPeekBufferPositive`, religando o modo analógico
  a cada despertar.
- Detecta o despertar pelo callback de energia (`kscePowerRegisterCallback`), pelo botão Power
  e por uma pausa longa do próprio loop.

Os parâmetros (botão, sensibilidade do analógico, janela de tempo, trajeto do gesto)
ficam no topo de `src/main.c`.

## Licença

MIT. Veja [LICENSE](LICENSE).
