# ComboPeel (PS Vita)

Plugin de kernel para PS Vita que desbloqueia a tela de bloqueio com um combo de botões,
fazendo a mesma animação de "descascar" (peel) do gesto com o dedo.

**Combo:** analógico esquerdo para baixo + X

## Como usar

1. Aperte o botão PS para acender a tela (aparece a tela de bloqueio com o relógio).
2. Incline o analógico esquerdo para baixo e aperte X.
3. O plugin arrasta um "dedo virtual" do canto superior direito até o centro, e a tela é desbloqueada.

### Proteções contra acionamento acidental

- O combo só funciona nos **20 segundos** depois que o Vita acorda do modo de espera
  (ou nos primeiros 2 minutos depois de ligar).
- Funciona **uma vez** por despertar.
- Se você tocar na tela, o combo é desativado até a próxima vez que o Vita dormir.
- É preciso apertar o combo (não adianta já estar segurando quando a tela acende).

## Instalação (recomendado: pelo VPK)

Testado para firmware **3.65 com Ensō (HENkaku)**. Precisa de "Enable Unsafe Homebrew" ativado nas HENkaku Settings.

1. Baixe o artefato **ComboPeel** na aba **Actions** (execução mais recente com ✓ verde). Dentro do .zip está o `ComboPeel.vpk`.
2. Copie o `ComboPeel.vpk` para o cartão e instale pelo VitaShell (X no arquivo › Install).
3. Abra o app **ComboPeel** e aperte **X (Instalar)**. Ele:
   - copia o plugin para `ur0:tai/combo_peel.skprx`;
   - faz backup do `config.txt` com data e hora no nome
     (ex.: `config_2026-11-16_13-14_antes-de-instalar-combopeel.bak`);
   - adiciona a linha na seção `*KERNEL`, sem duplicar.
4. Aperte **SELECT** para reiniciar.
5. A bolha pode ficar ou ser apagada: o plugin continua instalado nos dois casos.

> **Atenção:** o botão **Desinstalar** remove o plugin. Ele não restaura backup nenhum:
> só tira a linha do ComboPeel do `config.txt` atual e mantém todo o resto.
> Os backups nunca são usados automaticamente; servem só para você consultar se precisar.

Para remover o plugin: instale o VPK de novo, abra e aperte **Triângulo (Desinstalar)**.

### Instalação manual

1. Copie `combo_peel.skprx` para `ur0:tai/` (pelo VitaShell).
2. No `ur0:tai/config.txt`, adicione `ur0:tai/combo_peel.skprx` na seção `*KERNEL`.
3. Reinicie.

## Se algo der errado

- Se o Vita travar na inicialização, segure **L** enquanto liga para pular os plugins,
  e remova a linha do `config.txt`.

## Compilar

O GitHub Actions compila sozinho a cada envio. Para compilar localmente com o VitaSDK:

```
cmake -S . -B build
cmake --build build
```

## Como funciona

- Intercepta as funções de leitura do touch no kernel (`ksceTouchPeek/Read` e variantes `Region`)
  e, durante ~0,6 s, substitui os dados pelo gesto de um dedo virtual.
- Lê os botões com `ksceCtrlPeekBufferPositive`.
- Detecta o despertar com um callback de energia (`kscePowerRegisterCallback`).

Os parâmetros (botão, sensibilidade do analógico, janela de tempo, trajeto do gesto)
ficam no topo de `src/main.c`.
