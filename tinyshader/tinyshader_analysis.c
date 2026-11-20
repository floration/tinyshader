/**
 * This file is part of the tinyshader library.
 * See tinyshader.h for license details.
 */
#include "tinyshader_internal.h"

typedef struct Analyzer
{
    TsCompiler *compiler;
    Module *module;

    AstDecl *scope_func;
    ArrayOfScopePtr scope_stack;

    ArrayOfAstStmtPtr continue_stack;
    ArrayOfAstStmtPtr break_stack;

    uint32_t last_uniform_binding;

    // Function template instances, in the order they finished analysis
    ArrayOfAstDeclPtr template_instances;
} Analyzer;

static void scopeInit(TsCompiler *compiler, Scope *scope, Scope *parent, AstDecl *owner)
{
    memset(scope, 0, sizeof(*scope));
    ts__hashInit(compiler, &scope->map, 0);
    scope->parent = parent;
    scope->owner = owner;
}

static AstDecl *scopeGetLocal(Scope *scope, const char *name)
{
    AstDecl *symbol = NULL;
    bool found = ts__hashGet(&scope->map, name, (void **)&symbol);
    if (found) return symbol;
    return NULL;
}

static AstDecl *scopeGetGlobal(Scope *scope, const char *name)
{
    if (scope->parent)
    {
        AstDecl *sym = scopeGetGlobal(scope->parent, name);
        if (sym) return sym;
    }

    return scopeGetLocal(scope, name);
}

static bool scopeAdd(Scope *scope, const char *name, AstDecl *decl)
{
    TS_ASSERT(scope);

    if (scopeGetLocal(scope, name)) return false;

    ts__hashSet(&scope->map, name, decl);

    return true;
}

static char *typeToString(TsCompiler *compiler, AstType *type)
{
    char *prefix = NULL;
    char *storage_class = NULL;
    char *sub = NULL;
    char *postfix = NULL;

    switch (type->kind)
    {
    case TYPE_VOID: prefix = "void"; break;

    case TYPE_TYPE: prefix = "type"; break;

    case TYPE_BOOL: prefix = "bool"; break;

    case TYPE_FLOAT:
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "float%u", type->float_.bits);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;

    case TYPE_INT:
        ts__sbReset(&compiler->sb);

        if (type->int_.is_signed)
            ts__sbAppend(&compiler->sb, "int");
        else
            ts__sbAppend(&compiler->sb, "uint");

        ts__sbSprintf(&compiler->sb, "%u", type->int_.bits);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;

    case TYPE_VECTOR:
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "vec%u", type->vector.size);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);

        sub = typeToString(compiler, type->vector.elem_type);
        break;
    case TYPE_MATRIX:
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "mat%u", type->matrix.col_count);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);

        sub = typeToString(compiler, type->matrix.col_type);
        break;

    case TYPE_POINTER: {
        prefix = "ptr";

        switch (type->ptr.storage_class)
        {
        case SpvStorageClassInput: storage_class = "input"; break;
        case SpvStorageClassOutput: storage_class = "output"; break;
        case SpvStorageClassUniformConstant: storage_class = "constant"; break;
        case SpvStorageClassUniform: storage_class = "uniform"; break;
        case SpvStorageClassStorageBuffer: storage_class = "storage"; break;
        case SpvStorageClassFunction: storage_class = "function"; break;

        default: TS_ASSERT(0); break;
        }

        TS_ASSERT(type->ptr.sub);
        sub = typeToString(compiler, type->ptr.sub);
        break;
    }

    case TYPE_CONSTANT_BUFFER:
        prefix = "CB";
        sub = typeToString(compiler, type->buffer.sub);
        break;

    case TYPE_STRUCTURED_BUFFER:
        prefix = "SB";
        sub = typeToString(compiler, type->buffer.sub);
        break;

    case TYPE_RW_STRUCTURED_BUFFER:
        prefix = "RWSB";
        sub = typeToString(compiler, type->buffer.sub);
        break;

    case TYPE_RUNTIME_ARRAY:
        prefix = "r_array";
        sub = typeToString(compiler, type->array.sub);
        break;

    case TYPE_ARRAY:
    {
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "array%zu", type->array.size);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);

        sub = typeToString(compiler, type->array.sub);
        break;
    }

    case TYPE_FUNC: {
        prefix = "func";

        char *return_type = typeToString(compiler, type->func.return_type);
        char **params = NEW_ARRAY(compiler, char *, type->func.param_count);
        for (uint32_t i = 0; i < type->func.param_count; ++i)
        {
            params[i] = typeToString(compiler, type->func.params[i]);
        }

        ts__sbReset(&compiler->sb);
        ts__sbAppend(&compiler->sb, return_type);
        ts__sbAppend(&compiler->sb, "$");
        ts__sbSprintf(&compiler->sb, "%u", type->func.param_count);
        for (uint32_t i = 0; i < type->func.param_count; ++i)
        {
            if (i != 0) ts__sbAppend(&compiler->sb, "_");
            ts__sbAppend(&compiler->sb, params[i]);
        }
        sub = ts__sbBuild(&compiler->sb, &compiler->alloc);

        break;
    }

    case TYPE_STRUCT: {
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(
            &compiler->sb, "struct%zu%s", strlen(type->struct_.name), type->struct_.name);
        prefix = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_SAMPLER: {
        prefix = "sampler";
        break;
    }

    case TYPE_IMAGE: {
        prefix = "image";

        sub = typeToString(compiler, type->image.sampled_type);

        switch (type->image.dim)
        {
        case SpvDimCube: postfix = "Cube"; break;
        case SpvDim1D: postfix = "1D"; break;
        case SpvDim2D: postfix = "2D"; break;
        case SpvDim3D: postfix = "3D"; break;
        default: TS_ASSERT(0); break;
        }

        break;
    }
    }

    ts__sbReset(&compiler->sb);

    TS_ASSERT(prefix);
    ts__sbAppend(&compiler->sb, prefix);

    if (storage_class)
    {
        ts__sbAppend(&compiler->sb, "_");
        ts__sbAppend(&compiler->sb, storage_class);
    }

    if (sub)
    {
        ts__sbAppend(&compiler->sb, "_");
        ts__sbAppend(&compiler->sb, sub);
    }

    if (postfix)
    {
        ts__sbAppend(&compiler->sb, "_");
        ts__sbAppend(&compiler->sb, postfix);
    }

    type->string = ts__sbBuild(&compiler->sb, &compiler->alloc);
    return type->string;
}

char *ts__typeToPrettyString(TsCompiler *compiler, AstType *type)
{
    char *str = NULL;

    switch (type->kind)
    {
    case TYPE_VOID: str = "void"; break;
    case TYPE_TYPE: str = "type"; break;
    case TYPE_BOOL: str = "bool"; break;

    case TYPE_FLOAT:
        switch (type->float_.bits)
        {
        case 32: str = "float"; break;
        case 64: str = "double"; break;
        default: TS_ASSERT(0); break;
        }

        break;

    case TYPE_INT:
    {
        switch (type->float_.bits)
        {
        case 32:
        {
            if (type->int_.is_signed)
                str = "int";
            else
                str = "uint";
            break;
        }

        default: TS_ASSERT(0); break;
        }

        break;
    }

    case TYPE_VECTOR:
    {
        char *elem_str = ts__typeToPrettyString(compiler, type->vector.elem_type);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "%s%u", elem_str, type->vector.size);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_MATRIX:
    {
        char *elem_str = ts__typeToPrettyString(compiler,
                                                type->matrix.col_type->vector.elem_type);
        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb,
                      "%s%ux%u",
                      elem_str,
                      type->matrix.col_count,
                      type->matrix.col_type->vector.size);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_POINTER: {
        TS_ASSERT(0);
        break;
    }

    case TYPE_CONSTANT_BUFFER:
    {
        char *sub_str = ts__typeToPrettyString(compiler, type->buffer.sub);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "ConstantBuffer<%s>", sub_str);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_STRUCTURED_BUFFER:
    {
        char *sub_str = ts__typeToPrettyString(compiler, type->buffer.sub);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "StructuredBuffer<%s>", sub_str);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_RW_STRUCTURED_BUFFER:
    {
        char *sub_str = ts__typeToPrettyString(compiler, type->buffer.sub);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "RWStructuredBuffer<%s>", sub_str);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_RUNTIME_ARRAY:
    {
        char *sub_str = ts__typeToPrettyString(compiler, type->array.sub);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "%s[]", sub_str);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_ARRAY:
    {
        char *sub_str = ts__typeToPrettyString(compiler, type->array.sub);

        ts__sbReset(&compiler->sb);
        ts__sbSprintf(&compiler->sb, "%s[%zu]", sub_str, type->array.size);
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);
        break;
    }

    case TYPE_FUNC: {
        char *return_type = ts__typeToPrettyString(compiler, type->func.return_type);
        char **params = NEW_ARRAY(compiler, char *, type->func.param_count);
        for (uint32_t i = 0; i < type->func.param_count; ++i)
        {
            params[i] = ts__typeToPrettyString(compiler, type->func.params[i]);
        }

        ts__sbReset(&compiler->sb);
        ts__sbAppend(&compiler->sb, return_type);
        ts__sbAppend(&compiler->sb, "()(");
        for (uint32_t i = 0; i < type->func.param_count; ++i)
        {
            if (i != 0) ts__sbAppend(&compiler->sb, ", ");
            ts__sbAppend(&compiler->sb, params[i]);
        }
        ts__sbAppend(&compiler->sb, ")");
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);

        break;
    }

    case TYPE_STRUCT: {
        str = type->struct_.name;
        break;
    }

    case TYPE_SAMPLER: {
        str = "SamplerState";
        break;
    }

    case TYPE_IMAGE: {
        char *sub_str = ts__typeToPrettyString(compiler, type->image.sampled_type);

        ts__sbReset(&compiler->sb);

        switch (type->image.dim)
        {
        case SpvDimCube: ts__sbAppend(&compiler->sb, "TextureCube"); break;
        case SpvDim1D: ts__sbAppend(&compiler->sb, "Texture1D"); break;
        case SpvDim2D: ts__sbAppend(&compiler->sb, "Texture2D"); break;
        case SpvDim3D: ts__sbAppend(&compiler->sb, "Texture3D"); break;
        default: TS_ASSERT(0); break;
        }

        ts__sbAppend(&compiler->sb, "<");
        ts__sbAppend(&compiler->sb, sub_str);
        ts__sbAppend(&compiler->sb, ">");
        str = ts__sbBuild(&compiler->sb, &compiler->alloc);

        break;
    }
    }

    return str;
}

static uint32_t padToAlignment(uint32_t current, uint32_t align)
{
    TS_ASSERT(align >= 1);

    uint32_t minum = current & (align - 1);
    if (minum)
    {
        TS_ASSERT((current % align) != 0);
        current += align - minum;
    }

    return current;
}

static uint32_t typeAlignOf(Module *m, AstType *type)
{
    TS_ASSERT(type);
    if (type->align > 0) return type->align;

    uint32_t align = 1;
    switch (type->kind)
    {
    case TYPE_INT: align = type->int_.bits / 8; break;
    case TYPE_FLOAT: align = type->float_.bits / 8; break;

    case TYPE_CONSTANT_BUFFER: align = typeAlignOf(m, type->buffer.sub); break;
    case TYPE_STRUCTURED_BUFFER: align = typeAlignOf(m, type->buffer.sub); break;
    case TYPE_RW_STRUCTURED_BUFFER: align = typeAlignOf(m, type->buffer.sub); break;

    case TYPE_ARRAY: align = typeAlignOf(m, type->array.sub); break;

    case TYPE_VECTOR: {
        switch (type->vector.size)
        {
        case 2: align = typeAlignOf(m, type->vector.elem_type) * 2; break;
        case 3:
        case 4: align = typeAlignOf(m, type->vector.elem_type) * 4; break;
        default: TS_ASSERT(0); break;
        }

        break;
    }

    case TYPE_MATRIX: {
        align = typeAlignOf(m, type->matrix.col_type);
        break;
    }

    case TYPE_STRUCT: {
        for (AstType **field = type->struct_.fields;
             field != type->struct_.fields + type->struct_.field_count;
             ++field)
        {
            uint32_t field_align = typeAlignOf(m, *field);
            if (field_align > align) align = field_align;
        }

        break;
    }

    case TYPE_RUNTIME_ARRAY:
    case TYPE_IMAGE:
    case TYPE_SAMPLER:
    case TYPE_POINTER:
    case TYPE_BOOL:
    case TYPE_FUNC:
    case TYPE_VOID:
    case TYPE_TYPE: align = 0; break;
    }

    type->align = align;

    return type->align;
}

static uint32_t typeSizeOf(Module *m, AstType *type)
{
    TS_ASSERT(type);
    if (type->size > 0) return type->size;

    uint32_t size = 0;

    switch (type->kind)
    {
    case TYPE_BOOL: size = 1; break;
    case TYPE_INT: size = type->int_.bits / 8; break;
    case TYPE_FLOAT: size = type->float_.bits / 8; break;

    case TYPE_CONSTANT_BUFFER: size = typeSizeOf(m, type->buffer.sub); break;
    case TYPE_STRUCTURED_BUFFER: size = typeSizeOf(m, type->buffer.sub); break;
    case TYPE_RW_STRUCTURED_BUFFER: size = typeSizeOf(m, type->buffer.sub); break;

    case TYPE_ARRAY: size = typeSizeOf(m, type->array.sub) * type->array.size; break;

    case TYPE_VECTOR: {
        switch (type->vector.size)
        {
        case 2: size = typeSizeOf(m, type->vector.elem_type) * 2; break;
        case 3: size = typeSizeOf(m, type->vector.elem_type) * 3; break;
        case 4: size = typeSizeOf(m, type->vector.elem_type) * 4; break;
        default: TS_ASSERT(0); break;
        }
        break;
    }

    case TYPE_MATRIX: {
        size = typeSizeOf(m, type->matrix.col_type) * type->matrix.col_count;
        break;
    }

    case TYPE_STRUCT: {
        uint32_t struct_alignment = typeAlignOf(m, type);

        for (uint32_t i = 0; i < type->struct_.field_count; ++i)
        {
            AstType *field = type->struct_.fields[i];
            uint32_t field_align = typeAlignOf(m, field);
            size = padToAlignment(size, field_align); // Add padding

            IRMemberDecoration member_dec = {0};
            member_dec.kind = SpvDecorationOffset;
            member_dec.member_index = i;
            member_dec.value = size;
            arrPush(m->compiler, &type->struct_.field_decorations, member_dec);

            size += typeSizeOf(m, field);
        }

        // Add padding at the end of the struct
        if (size % struct_alignment > 0)
        {
            size += (struct_alignment - (size % struct_alignment));
        }

        break;
    }

    case TYPE_RUNTIME_ARRAY:
    case TYPE_IMAGE:
    case TYPE_SAMPLER:
    case TYPE_POINTER:
    case TYPE_FUNC:
    case TYPE_VOID:
    case TYPE_TYPE: size = 0; break;
    }

    type->size = size;
    return type->size;
}

static AstType *getCachedType(Module *m, AstType *type)
{
    char *type_string = typeToString(m->compiler, type);
    TS_ASSERT(type_string);
    TS_ASSERT(strlen(type_string) > 0);

    AstType *found_type = NULL;
    if (ts__hashGet(&m->type_cache, type_string, (void **)&found_type))
    {
        TS_ASSERT(found_type);
        return found_type;
    }

    ts__hashSet(&m->type_cache, type_string, type);

    typeSizeOf(m, type);

    return type;
}

static AstType *newBasicType(Module *m, AstTypeKind kind)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = kind;
    return getCachedType(m, ty);
}

static AstType *newPointerType(Module *m, SpvStorageClass storage_class, AstType *sub)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_POINTER;
    ty->ptr.storage_class = storage_class;
    ty->ptr.sub = sub;
    return getCachedType(m, ty);
}

static AstType *newArrayType(Module *m, AstType *elem_type, size_t size)
{
    TS_ASSERT(size > 0);
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_ARRAY;
    ty->array.sub = elem_type;
    ty->array.size = size;
    return getCachedType(m, ty);
}

static AstType *newRuntimeArrayType(Module *m, AstType *elem_type)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_RUNTIME_ARRAY;
    ty->array.sub = elem_type;
    return getCachedType(m, ty);
}

static AstType *newVectorType(Module *m, AstType *elem_type, uint32_t size)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_VECTOR;
    ty->vector.elem_type = elem_type;
    ty->vector.size = size;
    return getCachedType(m, ty);
}

static AstType *newMatrixType(Module *m, AstType *col_type, uint32_t col_count)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_MATRIX;
    ty->matrix.col_type = col_type;
    ty->matrix.col_count = col_count;
    return getCachedType(m, ty);
}

static AstType *newFloatType(Module *m, uint32_t bits)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_FLOAT;
    ty->float_.bits = bits;
    return getCachedType(m, ty);
}

static AstType *newIntType(Module *m, uint32_t bits, bool is_signed)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_INT;
    ty->int_.bits = bits;
    ty->int_.is_signed = is_signed;
    return getCachedType(m, ty);
}

static AstType *
newFuncType(Module *m, AstType *return_type, AstType **params, uint32_t param_count)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_FUNC;
    ty->func.return_type = return_type;
    if (param_count > 0)
    {
        ty->func.param_count = param_count;
        ty->func.params = NEW_ARRAY(m->compiler, AstType *, param_count);
        memcpy(ty->func.params, params, sizeof(AstType *) * param_count);
    }
    return getCachedType(m, ty);
}

static AstType *newStructType(
    Module *m, char *name, AstType **fields, AstDecl **field_decls, uint32_t field_count)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_STRUCT;
    ty->struct_.name = name;

    if (field_count > 0)
    {
        ty->struct_.field_count = field_count;

        ty->struct_.fields = NEW_ARRAY(m->compiler, AstType *, field_count);
        memcpy(ty->struct_.fields, fields, sizeof(AstType *) * field_count);

        ty->struct_.field_decls = NEW_ARRAY(m->compiler, AstDecl *, field_count);
        memcpy(ty->struct_.field_decls, field_decls, sizeof(AstDecl *) * field_count);
    }

    return getCachedType(m, ty);
}

static AstType *newImageType(Module *m, AstType *sampled_type, SpvDim dim)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_IMAGE;
    ty->image.sampled_type = sampled_type;
    ty->image.dim = dim;
    ty->image.depth = 0;
    ty->image.arrayed = 0;
    ty->image.multisampled = 0;
    ty->image.sampled = 1;
    ty->image.format = SpvImageFormatUnknown;
    return getCachedType(m, ty);
}

static AstType *newConstantBufferType(Module *m, AstType *sub)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_CONSTANT_BUFFER;
    ty->buffer.sub = sub;
    return getCachedType(m, ty);
}

static AstType *newStructuredBufferType(Module *m, AstType *sub)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_STRUCTURED_BUFFER;
    ty->buffer.sub = sub;
    return getCachedType(m, ty);
}

static AstType *newRWStructuredBufferType(Module *m, AstType *sub)
{
    AstType *ty = NEW(m->compiler, AstType);
    ty->kind = TYPE_RW_STRUCTURED_BUFFER;
    ty->buffer.sub = sub;
    return getCachedType(m, ty);
}

static bool isTypeCastable(AstType *src_type, AstType *dst_type)
{
    if (src_type == dst_type)
    {
        return true;
    }
    else if ((src_type->kind == TYPE_BOOL &&
              (dst_type->kind == TYPE_INT || dst_type->kind == TYPE_FLOAT)) ||
             (dst_type->kind == TYPE_BOOL &&
              (src_type->kind == TYPE_INT || src_type->kind == TYPE_FLOAT)))
    {
        return true;
    }
    else if (src_type->kind == TYPE_INT)
    {
        if (src_type->int_.is_signed)
        {
            if (dst_type->kind == TYPE_INT)
            {
                return true;
            }
            else if (dst_type->kind == TYPE_FLOAT)
            {
                return true;
            }
            else
            {
                return false;
            }
        }
        else
        {
            if (dst_type->kind == TYPE_INT)
            {
                return true;
            }
            else if (dst_type->kind == TYPE_FLOAT)
            {
                return true;
            }
            else
            {
                return false;
            }
        }
    }
    else if (src_type->kind == TYPE_FLOAT)
    {
        if (dst_type->kind == TYPE_INT)
        {
            return true;
        }
        else if (dst_type->kind == TYPE_FLOAT)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    TS_ASSERT(0);
}

AstType *ts__getScalarType(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_FLOAT:
    case TYPE_INT: {
        return type;
    }

    case TYPE_VECTOR: {
        return type->vector.elem_type;
    }

    default: break;
    }

    return NULL;
}

uint32_t ts__getTypeElemCount(AstType *type)
{
    if (!type) return 0;

    switch (type->kind)
    {
    case TYPE_FLOAT:
    case TYPE_BOOL:
    case TYPE_INT: {
        return 1;
    }

    case TYPE_VECTOR: {
        return type->vector.size;
    }

    default: break;
    }

    return 0;
}

AstType *ts__getScalarTypeNoVec(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_FLOAT:
    case TYPE_INT: {
        return type;
    }

    default: break;
    }

    return NULL;
}

// Returns a type that can be used in a comparison operation
AstType *ts__getComparableType(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_FLOAT:
    case TYPE_BOOL:
    case TYPE_INT: {
        return type;
    }

    default: break;
    }

    return NULL;
}

// Returns a type that can be used in a logical operation
AstType *ts__getLogicalType(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_BOOL:
    case TYPE_INT: {
        return type;
    }

    default: break;
    }

    return NULL;
}

AstType *ts__getElemType(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_VECTOR: {
        return type->vector.elem_type;
    }

    default: break;
    }

    return type;
}

AstType *ts__getStructType(AstType *type)
{
    if (!type) return NULL;

    switch (type->kind)
    {
    case TYPE_STRUCT: return type;
    case TYPE_CONSTANT_BUFFER: {
        return ts__getStructType(type->buffer.sub);
    }

    default: break;
    }

    return NULL;
}

static bool canCoerceExprToScalarType(Analyzer *a, AstExpr *expr, AstType *scalar_type)
{
    if (!scalar_type) return false;

    switch (expr->kind)
    {
    case EXPR_PRIMARY: {
        switch (expr->primary.token->kind)
        {
        case TOKEN_INT_LIT: {
            return scalar_type->kind == TYPE_FLOAT || scalar_type->kind == TYPE_INT;
        }

        default: break;
        }
        break;
    }

    case EXPR_BINARY: {
        switch (expr->binary.op)
        {
        case BINOP_ADD:
        case BINOP_SUB:
        case BINOP_MUL:
        case BINOP_DIV:
        case BINOP_MOD:

        case BINOP_GREATER:
        case BINOP_GREATEREQ:
        case BINOP_LESS:
        case BINOP_LESSEQ:
        case BINOP_EQ:
        case BINOP_NOTEQ: {
            return canCoerceExprToScalarType(a, expr->binary.left, scalar_type) &&
                   canCoerceExprToScalarType(a, expr->binary.right, scalar_type);
        }

        default: break;
        }

        break;
    }

    case EXPR_UNARY: {
        switch (expr->unary.op)
        {
        case UNOP_NEG: {
            return canCoerceExprToScalarType(a, expr->unary.right, scalar_type);
        }

        default: break;
        }

        break;
    }

    default: break;
    }

    return false;
}

static void tryCoerceExprToScalarType(Analyzer *a, AstExpr *expr, AstType *scalar_type)
{
    if (!scalar_type) return;

    switch (expr->kind)
    {
    case EXPR_PRIMARY: {
        if (canCoerceExprToScalarType(a, expr, scalar_type))
        {
            expr->type = scalar_type;
        }
        break;
    }

    case EXPR_BINARY: {
        if (canCoerceExprToScalarType(a, expr, scalar_type))
        {
            tryCoerceExprToScalarType(a, expr->binary.left, scalar_type);
            tryCoerceExprToScalarType(a, expr->binary.right, scalar_type);
            expr->type = scalar_type;
        }
        break;
    }

    case EXPR_UNARY: {
        if (canCoerceExprToScalarType(a, expr, scalar_type))
        {
            tryCoerceExprToScalarType(a, expr->unary.right, scalar_type);
            expr->type = scalar_type;
        }
        break;
    }

    default: break;
    }
}

static void analyzerAnalyzeExpr(Analyzer *a, AstExpr *expr, AstType *expected_type);

// Does a 'break' inside this statement leave the enclosing switch?
// Breaks nested in loops or inner switches belong to those instead.
static bool stmtBreaksOut(AstStmt *stmt)
{
    if (!stmt) return false;
    switch (stmt->kind)
    {
    case STMT_BREAK: return true;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->block.stmts.len; ++i)
            if (stmtBreaksOut(stmt->block.stmts.ptr[i])) return true;
        return false;
    case STMT_IF:
        return stmtBreaksOut(stmt->if_.if_stmt) || stmtBreaksOut(stmt->if_.else_stmt);
    default: return false;
    }
}

// Conservative: true only if control can never reach the end of 'stmt'.
static bool stmtAlwaysReturns(AstStmt *stmt)
{
    if (!stmt) return false;
    switch (stmt->kind)
    {
    case STMT_RETURN:
    case STMT_DISCARD: return true;
    case STMT_BLOCK:
        for (size_t i = 0; i < stmt->block.stmts.len; ++i)
            if (stmtAlwaysReturns(stmt->block.stmts.ptr[i])) return true;
        return false;
    case STMT_IF:
        return stmt->if_.else_stmt && stmtAlwaysReturns(stmt->if_.if_stmt) &&
               stmtAlwaysReturns(stmt->if_.else_stmt);
    case STMT_SWITCH: {
        bool has_default = false;
        for (size_t i = 0; i < stmt->switch_.labels.len; ++i)
            if (!stmt->switch_.labels.ptr[i].value) has_default = true;
        if (!has_default || stmtBreaksOut(stmt->switch_.body)) return false;
        // Without breaks every section falls through to the end, so the
        // switch returns iff its last statement does.
        ArrayOfAstStmtPtr stmts = stmt->switch_.body->block.stmts;
        return stmts.len > 0 && stmtAlwaysReturns(stmts.ptr[stmts.len - 1]);
    }
    default: return false;
    }
}

//
// Intrinsics that are expressible in terms of other HLSL constructs are
// rewritten here, before analysis, so codegen doesn't need to know them.
//
static AstExpr *mkFloatLit(TsCompiler *compiler, Location loc, double value)
{
    Token *tok = NEW(compiler, Token);
    tok->kind = TOKEN_FLOAT_LIT;
    tok->loc = loc;
    tok->double_ = value;

    AstExpr *e = NEW(compiler, AstExpr);
    e->kind = EXPR_PRIMARY;
    e->loc = loc;
    e->primary.token = tok;
    return e;
}

static AstExpr *mkBinary(TsCompiler *compiler, Location loc, AstBinaryOp op, AstExpr *l, AstExpr *r)
{
    AstExpr *e = NEW(compiler, AstExpr);
    e->kind = EXPR_BINARY;
    e->loc = loc;
    e->binary.op = op;
    e->binary.left = l;
    e->binary.right = r;
    return e;
}

static void setCall(TsCompiler *compiler, AstExpr *e, const char *name, AstExpr **args, size_t count)
{
    AstExpr *func = NEW(compiler, AstExpr);
    func->kind = EXPR_IDENT;
    func->loc = e->loc;
    func->ident.name = (char *)name;

    ArrayOfAstExprPtr params = {0};
    for (size_t i = 0; i < count; ++i) arrPush(compiler, &params, args[i]);

    e->kind = EXPR_FUNC_CALL;
    e->func_call.func_expr = func;
    e->func_call.params = params;
}

static bool desugarIntrinsic(TsCompiler *compiler, AstExpr *expr)
{
    AstExpr *func_expr = expr->func_call.func_expr;
    if (func_expr->kind != EXPR_IDENT) return false;

    const char *name = func_expr->ident.name;
    size_t n = arrLength(expr->func_call.params);
    AstExpr **p = expr->func_call.params.ptr;
    Location loc = expr->loc;

    if (strcmp(name, "saturate") == 0 && n == 1)
    {
        AstExpr *args[3] = {p[0], mkFloatLit(compiler, loc, 0.0), mkFloatLit(compiler, loc, 1.0)};
        setCall(compiler, expr, "clamp", args, 3);
        return true;
    }
    if (strcmp(name, "mad") == 0 && n == 3)
    {
        AstExpr *mul = mkBinary(compiler, loc, BINOP_MUL, p[0], p[1]);
        AstExpr *add = mkBinary(compiler, loc, BINOP_ADD, mul, p[2]);
        *expr = *add;
        return true;
    }
    if ((strcmp(name, "and") == 0 || strcmp(name, "or") == 0) && n == 2)
    {
        AstExpr *e = mkBinary(compiler, loc,
                              (name[0] == 'a') ? BINOP_LOGICAL_AND : BINOP_LOGICAL_OR, p[0], p[1]);
        *expr = *e;
        return true;
    }
    if (strcmp(name, "select") == 0 && n == 3)
    {
        AstExpr *e = NEW(compiler, AstExpr);
        e->kind = EXPR_TERNARY;
        e->loc = loc;
        e->ternary.cond = p[0];
        e->ternary.true_expr = p[1];
        e->ternary.false_expr = p[2];
        *expr = *e;
        return true;
    }
    if (strcmp(name, "rcp") == 0 && n == 1)
    {
        AstExpr *div = mkBinary(compiler, loc, BINOP_DIV, mkFloatLit(compiler, loc, 1.0), p[0]);
        *expr = *div;
        return true;
    }
    if (strcmp(name, "log10") == 0 && n == 1)
    {
        AstExpr *arg = p[0];
        AstExpr *call = NEW(compiler, AstExpr);
        call->loc = loc;
        setCall(compiler, call, "log2", &arg, 1);
        AstExpr *mul =
            mkBinary(compiler, loc, BINOP_MUL, call, mkFloatLit(compiler, loc, 0.30102999566398120));
        *expr = *mul;
        return true;
    }

    return false;
}


static bool isNumericOrBool(AstType *t)
{
    return t && (t->kind == TYPE_FLOAT || t->kind == TYPE_INT || t->kind == TYPE_BOOL);
}

// C-style casts. Scalars/vectors convert element-wise, a scalar splats to a
// vector, a vector truncates to a smaller vector or scalar, and "(Struct)0"
// zero-initializes. All of these lower through EXPR_AUTO_CAST.
static void analyzerAnalyzeCast(Analyzer *a, AstExpr *expr, AstType *target)
{
    TsCompiler *compiler = a->compiler;
    AstExpr *operand = expr->func_call.params.ptr[0];
    analyzerAnalyzeExpr(a, operand, NULL);
    expr->type = NULL;
    if (!operand->type) return;

    AstType *src = operand->type;
    AstType *src_scalar = (src->kind == TYPE_VECTOR) ? src->vector.elem_type : src;
    AstType *dst_scalar = (target->kind == TYPE_VECTOR) ? target->vector.elem_type : target;

    bool ok = false;
    if (src_scalar && dst_scalar && isNumericOrBool(src_scalar) && isNumericOrBool(dst_scalar))
    {
        uint32_t src_n = (src->kind == TYPE_VECTOR) ? src->vector.size : 1;
        uint32_t dst_n = (target->kind == TYPE_VECTOR) ? target->vector.size : 1;
        // splat (1 -> n), same size, or truncation (n -> fewer)
        ok = (src_n == 1) || (src_n >= dst_n);
    }
    else if ((target->kind == TYPE_STRUCT || target->kind == TYPE_ARRAY ||
              target->kind == TYPE_MATRIX) &&
             src->kind != TYPE_VECTOR && isNumericOrBool(src))
    {
        // "(T)scalar" fills every member with the scalar ("(T)0" zero-initializes)
        ok = true;
    }
    else if (src == target)
    {
        ok = true;
    }

    if (!ok)
    {
        ts__addErr(compiler, &expr->loc, "invalid cast");
        return;
    }

    Location loc = expr->loc;
    memset(expr, 0, sizeof(*expr));
    expr->kind = EXPR_AUTO_CAST;
    expr->loc = loc;
    expr->auto_cast.sub = operand;
    expr->type = target;
}

static void analyzerAnalyzeDecl(Analyzer *a, AstDecl *decl);
static void analyzerPushScope(Analyzer *a, Scope *scope);
static void analyzerPopScope(Analyzer *a, Scope *scope);

// Is this type expression just the name of a template parameter? Returns its index.
static int templateParamIndex(AstDecl *tmpl, AstExpr *type_expr)
{
    if (!type_expr || type_expr->kind != EXPR_IDENT) return -1;
    for (uint32_t i = 0; i < tmpl->func.template_param_count; ++i)
    {
        if (strcmp(tmpl->func.template_params[i], type_expr->ident.name) == 0) return (int)i;
    }
    return -1;
}

// Calling a function template: deduce its parameters from the argument types,
// instantiate (or reuse) "name<types>", and point the call at the instance.
static void analyzerInstantiateTemplateCall(Analyzer *a, AstExpr *expr, AstDecl *tmpl)
{
    TsCompiler *compiler = a->compiler;
    Module *m = a->module;

    uint32_t arg_count = (uint32_t)arrLength(expr->func_call.params);
    if (arg_count != arrLength(tmpl->func.params))
    {
        ts__addErr(compiler, &expr->loc, "wrong amount of parameters for function call");
        return;
    }

    uint32_t n = tmpl->func.template_param_count;
    AstType **bound = NEW_ARRAY(compiler, AstType *, n);

    for (uint32_t i = 0; i < arg_count; ++i)
    {
        AstExpr *arg = expr->func_call.params.ptr[i];
        analyzerAnalyzeExpr(a, arg, NULL);
        if (!arg->type) return;

        int idx = templateParamIndex(tmpl, tmpl->func.params.ptr[i]->var.type_expr);
        if (idx < 0) continue;

        if (!bound[idx])
        {
            bound[idx] = arg->type;
        }
        else if (bound[idx] != arg->type)
        {
            ts__addErr(compiler, &arg->loc,
                       "conflicting types deduced for template parameter '%s': '%s' and '%s'",
                       tmpl->func.template_params[idx],
                       ts__typeToPrettyString(compiler, bound[idx]),
                       ts__typeToPrettyString(compiler, arg->type));
            return;
        }
    }

    // Type names first: ts__typeToPrettyString uses the shared string builder
    char **type_names = NEW_ARRAY(compiler, char *, n);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!bound[i])
        {
            ts__addErr(compiler, &expr->loc,
                       "could not deduce template parameter '%s' from the arguments",
                       tmpl->func.template_params[i]);
            return;
        }
        type_names[i] = ts__typeToPrettyString(compiler, bound[i]);
    }

    ts__sbReset(&compiler->sb);
    ts__sbAppend(&compiler->sb, tmpl->name);
    ts__sbAppend(&compiler->sb, "<");
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i > 0) ts__sbAppend(&compiler->sb, ",");
        ts__sbAppend(&compiler->sb, type_names[i]);
    }
    ts__sbAppend(&compiler->sb, ">");
    char *mangled = ts__sbBuild(&compiler->sb, &compiler->alloc);

    if (!scopeGetLocal(m->scope, mangled))
    {
        AstDecl *instance = ts__parseTemplateInstance(compiler, tmpl);
        if (!instance) return;
        instance->name = mangled;
        scopeAdd(m->scope, mangled, instance);

        // Template parameters are visible only inside the instance
        Scope *tmpl_scope = NEW(compiler, Scope);
        scopeInit(compiler, tmpl_scope, m->scope, NULL);
        for (uint32_t i = 0; i < n; ++i)
        {
            AstDecl *alias = NEW(compiler, AstDecl);
            alias->kind = DECL_TYPE_ALIAS;
            alias->loc = tmpl->loc;
            alias->name = tmpl->func.template_params[i];
            alias->type = newBasicType(m, TYPE_TYPE);
            alias->as_type = bound[i];
            scopeAdd(tmpl_scope, alias->name, alias);
        }

        // Analyze it as if at top level, then restore the caller's state
        ArrayOfScopePtr saved_scopes = a->scope_stack;
        ArrayOfAstStmtPtr saved_break = a->break_stack;
        ArrayOfAstStmtPtr saved_continue = a->continue_stack;
        AstDecl *saved_func = a->scope_func;
        memset(&a->scope_stack, 0, sizeof(a->scope_stack));
        memset(&a->break_stack, 0, sizeof(a->break_stack));
        memset(&a->continue_stack, 0, sizeof(a->continue_stack));
        a->scope_func = NULL;

        analyzerPushScope(a, m->scope);
        analyzerPushScope(a, tmpl_scope);
        analyzerAnalyzeDecl(a, instance);
        analyzerPopScope(a, tmpl_scope);
        analyzerPopScope(a, m->scope);

        a->scope_stack = saved_scopes;
        a->break_stack = saved_break;
        a->continue_stack = saved_continue;
        a->scope_func = saved_func;

        // After its own analysis, so instances it uses come first
        arrPush(compiler, &a->template_instances, instance);
    }

    expr->func_call.func_expr->ident.name = mangled;
}

// Wraps an already-analyzed expression in an implicit conversion to 'type'.
static void wrapAutoCastM(TsCompiler *compiler, AstExpr *expr, AstType *type)
{
    if (!expr->type || expr->type == type) return;

    AstExpr *sub_expr = NEW(compiler, AstExpr);
    *sub_expr = *expr;

    expr->kind = EXPR_AUTO_CAST;
    expr->auto_cast.sub = sub_expr;
    expr->type = type;
    expr->assignable = false;
    expr->resolved_int = NULL;
}

static void wrapAutoCast(Analyzer *a, AstExpr *expr, AstType *type)
{
    wrapAutoCastM(a->compiler, expr, type);
}

// HLSL usual arithmetic conversions for scalar element types:
// any float beats any int, wider beats narrower, unsigned beats signed.
static AstType *commonScalarType(AstType *x, AstType *y)
{
    if (!x) return y;
    if (!y) return x;
    if (x == y) return x;

    if (x->kind == TYPE_FLOAT && y->kind != TYPE_FLOAT) return x;
    if (y->kind == TYPE_FLOAT && x->kind != TYPE_FLOAT) return y;

    if (x->kind == TYPE_FLOAT && y->kind == TYPE_FLOAT)
    {
        return (x->float_.bits >= y->float_.bits) ? x : y;
    }

    if (x->kind == TYPE_INT && y->kind == TYPE_INT)
    {
        if (x->int_.bits != y->int_.bits) return (x->int_.bits > y->int_.bits) ? x : y;
        return x->int_.is_signed ? y : x;
    }

    return NULL;
}

// Makes the arguments of a component-wise intrinsic agree: scalars are
// splatted to the widest vector argument and element types are promoted
// (to float when force_float is set), as DXC does.
static void unifyIntrinsicArgs(Analyzer *a, AstExpr **args, size_t count, bool force_float)
{
    Module *m = a->module;
    uint32_t vec_size = 0;
    AstType *common = force_float ? newFloatType(m, 32) : NULL;

    for (size_t i = 0; i < count; ++i)
    {
        AstType *t = args[i]->type;
        if (!t) return;
        AstType *st = ts__getScalarType(t);
        if (!st) return; // matrices, structs, etc.: leave to the existing checks
        if (t->kind == TYPE_VECTOR && t->vector.size > vec_size) vec_size = t->vector.size;
        common = commonScalarType(common, st);
        if (!common) return;
    }

    AstType *target = vec_size ? newVectorType(m, common, vec_size) : common;
    for (size_t i = 0; i < count; ++i)
    {
        AstType *t = args[i]->type;
        // Never narrow a vector argument implicitly here
        if (t->kind == TYPE_VECTOR && t->vector.size != vec_size) return;
    }
    for (size_t i = 0; i < count; ++i)
    {
        wrapAutoCast(a, args[i], target);
    }
}

// Converts the operands of a binary expression so their scalar element
// types match, keeping each operand's own shape (scalar or vector).
static void promoteBinaryOperands(Analyzer *a, AstExpr *expr)
{
    Module *m = a->module;
    AstExpr *l = expr->binary.left;
    AstExpr *r = expr->binary.right;
    if (!l->type || !r->type) return;

    AstType *ls = ts__getScalarType(l->type);
    AstType *rs = ts__getScalarType(r->type);
    if (!ls || !rs || ls == rs) return;

    AstType *common = commonScalarType(ls, rs);
    if (!common) return;

    if (ls != common)
    {
        wrapAutoCast(a, l, (l->type->kind == TYPE_VECTOR)
            ? newVectorType(m, common, l->type->vector.size) : common);
    }
    if (rs != common)
    {
        wrapAutoCast(a, r, (r->type->kind == TYPE_VECTOR)
            ? newVectorType(m, common, r->type->vector.size) : common);
    }
}

static void analyzerAnalyzeStmt(Analyzer *a, AstStmt *stmt);
static void analyzerAnalyzeDecl(Analyzer *a, AstDecl *decl);

static void analyzerPushScope(Analyzer *a, Scope *scope)
{
    TS_ASSERT(scope);
    arrPush(a->compiler, &a->scope_stack, scope);
    if (scope->owner && scope->owner->kind == DECL_FUNC)
    {
        a->scope_func = scope->owner;
    }
}

static void analyzerPopScope(Analyzer *a, Scope *scope)
{
    TS_ASSERT(scope);
    TS_ASSERT(arrLength(a->scope_stack) > 0);

    Scope *last_scope = a->scope_stack.ptr[arrLength(a->scope_stack) - 1];
    TS_ASSERT(last_scope == scope);

    arrPop(&a->scope_stack);

    a->scope_func = NULL;
    for (uint32_t i = 0; i < arrLength(a->scope_stack); ++i)
    {
        if (a->scope_stack.ptr[i]->owner &&
            a->scope_stack.ptr[i]->owner->kind == DECL_FUNC)
        {
            a->scope_func = a->scope_stack.ptr[i]->owner;
        }
    }
}

static Scope *analyzerCurrentScope(Analyzer *a)
{
    if (arrLength(a->scope_stack) > 0)
    {
        return a->scope_stack.ptr[arrLength(a->scope_stack) - 1];
    }
    return NULL;
}

static void analyzerTryRegisterDecl(Analyzer *a, AstDecl *decl)
{
    if (!decl->name) return;

    Scope *scope = analyzerCurrentScope(a);

    if (scopeGetLocal(scope, decl->name))
    {
        ts__addErr(a->compiler, &decl->loc, "duplicate declaration: '%s'", decl->name);
    }
    else
    {
        scopeAdd(scope, decl->name, decl);
    }
}

static void analyzerRecursivelyCheckForSemanticStrings(
    Analyzer *a, AstDecl *decl)
{
    TsCompiler *compiler = a->compiler;

    switch (decl->kind)
    {
    case DECL_FUNC:
    {
        for (uint32_t i = 0; i < decl->func.params.len; ++i)
        {
            AstDecl *param_decl = decl->func.params.ptr[i];
            analyzerRecursivelyCheckForSemanticStrings(a, param_decl);
        }

        AstType *return_type = decl->func.return_type->as_type;
        if (return_type->kind != TYPE_VOID)
        {
            if (return_type->kind == TYPE_STRUCT)
            {
                for (uint32_t i = 0; i < return_type->struct_.field_count; ++i)
                {
                    AstDecl *struct_field_decl = return_type->struct_.field_decls[i];
                    analyzerRecursivelyCheckForSemanticStrings(a, struct_field_decl);
                }
            }
            else if (!decl->semantic)
            {
                ts__addErr(
                    compiler,
                    &decl->loc,
                    "function return value needs a semantic string");
            }
        }
        break;
    }

    case DECL_VAR:
    {
        AstType *var_type = decl->type;
        TS_ASSERT(var_type);

        if (var_type->kind == TYPE_STRUCT)
        {
            for (uint32_t i = 0; i < var_type->struct_.field_count; ++i)
            {
                AstDecl *struct_field_decl = var_type->struct_.field_decls[i];
                analyzerRecursivelyCheckForSemanticStrings(a, struct_field_decl);
            }
        }
        else if (!decl->semantic)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "variable declaration needs a semantic string");
        }
        break;
    }

    case DECL_TYPE_ALIAS:

    case DECL_STRUCT_FIELD:
    {
        if (!decl->semantic)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "struct field declaration needs a semantic string");
        }
        break;
    }

    default: break;
    }
}

static void analyzerAnalyzeExpr(Analyzer *a, AstExpr *expr, AstType *expected_type)
{
    TS_ASSERT(expr);

    TsCompiler *compiler = a->compiler;
    Scope *scope = analyzerCurrentScope(a);
    Module *m = a->module;

    expr->inhabited_scope = scope;

    switch (expr->kind)
    {
    case EXPR_PRIMARY: {
        switch (expr->primary.token->kind)
        {
        case TOKEN_VOID: {
            expr->as_type = newBasicType(a->module, TYPE_VOID);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_BOOL: {
            expr->as_type = newBasicType(a->module, TYPE_BOOL);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_FLOAT: {
            expr->as_type = newFloatType(a->module, 32);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_INT: {
            expr->as_type = newIntType(a->module, 32, true);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_UINT: {
            expr->as_type = newIntType(a->module, 32, false);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_VECTOR_TYPE: {
            AstType *elem_type = NULL;

            switch (expr->primary.token->vector_type.elem_type)
            {
            case TOKEN_FLOAT: {
                elem_type = newFloatType(a->module, 32);
                break;
            }

            case TOKEN_INT: {
                elem_type = newIntType(a->module, 32, true);
                break;
            }

            case TOKEN_UINT: {
                elem_type = newIntType(a->module, 32, false);
                break;
            }

            case TOKEN_BOOL: {
                elem_type = newBasicType(a->module, TYPE_BOOL);
                break;
            }

            default: TS_ASSERT(0); break;
            }

            TS_ASSERT(elem_type);

            expr->as_type = newVectorType(
                a->module, elem_type, (uint32_t)expr->primary.token->vector_type.dim);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_MATRIX_TYPE: {
            AstType *elem_type = NULL;

            switch (expr->primary.token->matrix_type.elem_type)
            {
            case TOKEN_FLOAT: {
                elem_type = newFloatType(a->module, 32);
                break;
            }

            case TOKEN_INT: {
                elem_type = newIntType(a->module, 32, true);
                break;
            }

            case TOKEN_UINT: {
                elem_type = newIntType(a->module, 32, false);
                break;
            }

            default: TS_ASSERT(0); break;
            }

            TS_ASSERT(elem_type);

            AstType *col_type = newVectorType(
                a->module, elem_type, (uint32_t)expr->primary.token->matrix_type.dim1);
            expr->as_type = newMatrixType(
                a->module, col_type, (uint32_t)expr->primary.token->matrix_type.dim2);
            expr->type = newBasicType(a->module, TYPE_TYPE);
            break;
        }

        case TOKEN_FALSE:
        case TOKEN_TRUE: {
            expr->type = newBasicType(a->module, TYPE_BOOL);
            break;
        }

        case TOKEN_FLOAT_LIT: {
            if (expected_type)
            {
                if ((expected_type->kind == TYPE_FLOAT) || (expected_type->kind == TYPE_BOOL))
                {
                    expr->type = expected_type;
                }
            }
            
            if (!expr->type)
            {
                expr->type = newFloatType(a->module, 32);
            }
            break;
        }

        case TOKEN_INT_LIT: {
            if (expected_type)
            {
                if ((expected_type->kind == TYPE_INT) ||
                    (expected_type->kind == TYPE_FLOAT) ||
                    (expected_type->kind == TYPE_BOOL))
                {
                    expr->type = expected_type;
                }
            }

            if (!expr->type)
            {
                expr->type = newIntType(a->module, 32, true);
            }

            expr->resolved_int = NEW(compiler, int64_t);
            *expr->resolved_int = expr->primary.token->int_;

            break;
        }

        default: TS_ASSERT(0); break;
        }
        break;
    }

    case EXPR_VAR_ASSIGN: {
        AstExpr *assigned_expr = expr->var_assign.assigned_expr;

        analyzerAnalyzeExpr(a, assigned_expr, NULL);
        if (!assigned_expr->type)
        {
            ts__addErr(
                compiler,
                &assigned_expr->loc,
                "could not resolve type for expression");
            break;
        }

        expr->type = assigned_expr->type;

        if (!assigned_expr->assignable)
        {
            if (assigned_expr->kind == EXPR_IDENT &&
                assigned_expr->ident.decl->kind == DECL_VAR &&
                ((assigned_expr->ident.decl->var.type_modifiers & TYPE_MODIFIER_CONST) ||
                 (assigned_expr->ident.decl->var.storage_class == VAR_STORAGE_CLASS_UNIFORM)))
            {
                ts__addErr(
                    compiler,
                    &assigned_expr->loc,
                    "assigned variable is constant: '%s'",
                    assigned_expr->ident.name);
            }
            else
            {
                ts__addErr(
                    compiler,
                    &assigned_expr->loc,
                    "expression is not assignable");
            }
        }

        analyzerAnalyzeExpr(a, expr->var_assign.value_expr, assigned_expr->type);
        break;
    }

    case EXPR_IDENT: {
        AstDecl *decl = scopeGetGlobal(scope, expr->ident.name);
        if (!decl)
        {
            ts__addErr(compiler, &expr->loc, "unknown identifier: '%s'", expr->ident.name);
            break;
        }

        if (decl->kind == DECL_FUNC)
        {
            decl->func.called = true;
        }

        if (decl->kind == DECL_VAR)
        {
            expr->assignable = true;
            if (decl->var.storage_class == VAR_STORAGE_CLASS_UNIFORM)
            {
                expr->assignable = false;
            }
            if (decl->var.type_modifiers & TYPE_MODIFIER_CONST)
            {
                expr->assignable = false;
            }
        }

        expr->ident.decl = decl;

        expr->type = decl->type;
        expr->as_type = decl->as_type;
        expr->scope = decl->scope;
        expr->resolved_int = decl->resolved_int;
        break;
    }

    case EXPR_SUBSCRIPT: {
        AstExpr *left = expr->subscript.left;
        analyzerAnalyzeExpr(a, left, NULL);

        AstExpr *right = expr->subscript.right;
        analyzerAnalyzeExpr(a, right, NULL);

        if (!left->type || !right->type) break;

        switch (left->type->kind)
        {
        case TYPE_STRUCTURED_BUFFER: {
            expr->assignable = false;
            expr->type = left->type->buffer.sub;
            break;
        }
        case TYPE_RW_STRUCTURED_BUFFER: {
            expr->assignable = true;
            expr->type = left->type->buffer.sub;
            break;
        }
        case TYPE_VECTOR: {
            expr->assignable = left->assignable;
            expr->type = left->type->vector.elem_type;
            if (!right->resolved_int)
            {
                ts__addErr(compiler, &right->loc, "index is not an integer constant");
            }
            else if ((*right->resolved_int) >= left->type->vector.size)
            {
                ts__addErr(compiler, &right->loc, "index is out of bounds");
            }
            break;
        }
        case TYPE_MATRIX: {
            expr->assignable = left->assignable;
            expr->type = left->type->matrix.col_type;
            if (!right->resolved_int)
            {
                ts__addErr(compiler, &right->loc, "index is not an integer constant");
            }
            else if ((*right->resolved_int) >= left->type->matrix.col_count || (*right->resolved_int) < 0)
            {
                ts__addErr(
                    compiler,
                    &right->loc,
                    "index '%ld' is out of matrix bounds '%u'",
                    *right->resolved_int,
                    left->type->matrix.col_count);
            }
            break;
        }
        case TYPE_ARRAY:
        {
            expr->assignable = left->assignable;
            expr->type = left->type->array.sub;
            if (right->resolved_int &&
                ((*right->resolved_int) >= (int64_t)left->type->array.size ||
                 (*right->resolved_int) < 0))
            {
                ts__addErr(
                    compiler,
                    &right->loc,
                    "index '%ld' is out of array bounds '%zu'",
                    *right->resolved_int,
                    left->type->array.size);
            }
            break;
        }
        case TYPE_RUNTIME_ARRAY:
        {
            expr->assignable = left->assignable;
            expr->type = left->type->array.sub;
            break;
        }
        default: {
            ts__addErr(compiler, &left->loc, "expression is not subscriptable");
            break;
        }
        }

        AstType *struct_type = ts__getStructType(expr->type);
        if (struct_type)
        {
            expr->scope = NEW(compiler, Scope);
            scopeInit(compiler, expr->scope, NULL, NULL);
            for (uint32_t i = 0; i < struct_type->struct_.field_count; ++i)
            {
                AstDecl *field_decl = struct_type->struct_.field_decls[i];
                scopeAdd(expr->scope, field_decl->name, field_decl);
            }
        }

        if (right->type->kind != TYPE_INT)
        {
            ts__addErr(compiler, &right->loc, "subscript index must be of integer type");
            break;
        }

        break;
    }

    case EXPR_ACCESS: {
        AstExpr *left = expr->access.base;
        analyzerAnalyzeExpr(a, left, NULL);

        TS_ASSERT(expr->access.chain.len > 0);

        for (uint32_t i = 0; i < expr->access.chain.len; ++i)
        {
            AstExpr *right = expr->access.chain.ptr[i];

            if (!left->type)
            {
                TS_ASSERT(compiler->errors.len > 0);
                break;
            }

            AstType *struct_type = ts__getStructType(left->type);

            if (struct_type)
            {
                TS_ASSERT(left->scope);

                analyzerPushScope(a, left->scope);
                analyzerAnalyzeExpr(a, right, NULL);
                analyzerPopScope(a, left->scope);
            }
            else if (left->type->kind == TYPE_VECTOR)
            {
                TS_ASSERT(right->kind == EXPR_IDENT);

                char *selector = right->ident.name;
                size_t new_vec_dim = strlen(selector);
                if (new_vec_dim > 4)
                {
                    ts__addErr(
                        compiler,
                        &right->loc,
                        "vector shuffle must select at most 4 elements");
                    break;
                }

                uint32_t *positions = NEW_ARRAY(compiler, uint32_t, new_vec_dim);

                for (uint32_t j = 0; j < new_vec_dim; ++j)
                {
                    bool valid = true;
                    switch (selector[j])
                    {
                    case 'r':
                    case 'x': positions[j] = 0; break;
                    case 'g':
                    case 'y': positions[j] = 1; break;
                    case 'b':
                    case 'z': positions[j] = 2; break;
                    case 'a':
                    case 'w': positions[j] = 3; break;

                    default:
                        ts__addErr(compiler, &right->loc, "invalid vector shuffle: '%s'", selector);
                        valid = false;
                        break;
                    }

                    if (positions[j] >= left->type->vector.size)
                    {
                        ts__addErr(compiler, &right->loc, "invalid vector shuffle: '%s'", selector);
                        valid = false;
                        break;
                    }

                    if (!valid) break;
                }

                right->ident.shuffle_indices = positions;
                right->ident.shuffle_index_count = (uint32_t)new_vec_dim;

                if (new_vec_dim == 1)
                {
                    right->type = left->type->vector.elem_type;
                }
                else
                {
                    right->type = newVectorType(
                        m, left->type->vector.elem_type, (uint32_t)new_vec_dim);
                }
            }
            else
            {
                ts__addErr(compiler, &left->loc, "expression is not accessible");
                break;
            }

            if (i == (arrLength(expr->access.chain) - 1))
            {
                expr->type = right->type;
                expr->as_type = right->as_type;
                expr->scope = right->scope;
                expr->resolved_int = right->resolved_int;
            }

            left = right;
        }

        expr->assignable = expr->access.base->assignable;

        break;
    }

    case EXPR_FUNC_CALL: {
        if (desugarIntrinsic(compiler, expr))
        {
            analyzerAnalyzeExpr(a, expr, expected_type);
            return;
        }

        AstExpr *func_expr = expr->func_call.func_expr;

        if (func_expr->kind == EXPR_IDENT)
        {
            AstDecl *callee = scopeGetGlobal(scope, func_expr->ident.name);
            if (callee && callee->kind == DECL_FUNC && callee->func.template_param_count)
            {
                analyzerInstantiateTemplateCall(a, expr, callee);
                if (func_expr->ident.name == callee->name) break; // failed; error reported
            }
        }

        bool is_builtin_func = false;
        AstBuiltinFunction builtin_func_kind;

        // Builtin function
        if (func_expr->kind == EXPR_IDENT)
        {
            void *result;
            is_builtin_func = ts__hashGet(
                &compiler->builtin_function_table,
                func_expr->ident.name,
                &result);
            if (is_builtin_func)
            {
                builtin_func_kind = (AstBuiltinFunction)result;
            }
        }

        if (is_builtin_func)
        {
            uint32_t param_count = arrLength(expr->func_call.params);
            ArrayOfAstExprPtr params = expr->func_call.params;

            bool got_param_types = true;

            for (uint32_t i = 0; i < param_count; ++i)
            {
                AstExpr *param = params.ptr[i];
                analyzerAnalyzeExpr(a, param, NULL);
                if (!param->type)
                {
                    got_param_types = false;
                    continue;
                }
            }

            if (!got_param_types) break;

            switch (builtin_func_kind)
            {
            case AST_BUILTIN_FUNC_ANY:
            case AST_BUILTIN_FUNC_ALL:
            case AST_BUILTIN_FUNC_ISNAN:
            case AST_BUILTIN_FUNC_ISINF: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "this function takes 1 parameter");
                    break;
                }
                AstExpr *arg = params.ptr[0];
                if (!arg->type) break;

                AstType *elem = (arg->type->kind == TYPE_VECTOR) ? arg->type->vector.elem_type
                                                                 : arg->type;
                bool is_test = builtin_func_kind == AST_BUILTIN_FUNC_ISNAN ||
                               builtin_func_kind == AST_BUILTIN_FUNC_ISINF;
                bool ok = is_test ? elem->kind == TYPE_FLOAT
                                  : (elem->kind == TYPE_FLOAT || elem->kind == TYPE_INT ||
                                     elem->kind == TYPE_BOOL);
                if (!ok)
                {
                    ts__addErr(compiler, &arg->loc,
                               is_test ? "expected a float scalar or vector"
                                       : "expected a scalar or vector");
                    break;
                }

                AstType *bool_type = newBasicType(m, TYPE_BOOL);
                if (is_test && arg->type->kind == TYPE_VECTOR)
                    expr->type = newVectorType(m, bool_type, arg->type->vector.size);
                else
                    expr->type = bool_type;
                break;
            }

            case AST_BUILTIN_FUNC_DOT: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "dot needs 2 parameters");
                    break;
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];

                if ((a->type->kind != TYPE_VECTOR) || (b->type->kind != TYPE_VECTOR))
                {
                    ts__addErr(compiler, &expr->loc, "dot operates on vectors");
                    break;
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "dot cannot operate on different types");
                    break;
                }

                expr->type = a->type->vector.elem_type;
                break;
            }

            case AST_BUILTIN_FUNC_CROSS: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "cross needs 2 parameters");
                    break;
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];

                if ((a->type->kind != TYPE_VECTOR) || (b->type->kind != TYPE_VECTOR))
                {
                    ts__addErr(compiler, &expr->loc, "cross operates on vectors");
                    break;
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "cross cannot operate on different types");
                    break;
                }

                if (a->type->vector.size != 3)
                {
                    ts__addErr(compiler, &expr->loc, "cross operates on 3D vectors");
                    break;
                }

                expr->type = a->type;
                break;
            }

            case AST_BUILTIN_FUNC_LENGTH: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "length needs 1 parameter");
                    break;
                }

                AstExpr *a = params.ptr[0];

                if (a->type->kind != TYPE_VECTOR)
                {
                    ts__addErr(compiler, &expr->loc, "length operates on a vector");
                    break;
                }

                expr->type = a->type->vector.elem_type;
                break;
            }

            case AST_BUILTIN_FUNC_NORMALIZE: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "normalize needs 1 parameter");
                    break;
                }

                AstExpr *a = params.ptr[0];

                if (a->type->kind != TYPE_VECTOR)
                {
                    ts__addErr(compiler, &expr->loc, "normalize operates on a vector");
                    break;
                }

                expr->type = a->type;
                break;
            }

            case AST_BUILTIN_FUNC_MUL: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "mul needs 2 parameters");
                    break;
                }
                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                if (!a->type || !b->type) break;

                // HLSL floatRxC is stored as a SPIR-V matrix of R columns, each a
                // C-vector (the transpose), so col_count = R and col_type = vector(C).
                if (a->type->kind == TYPE_VECTOR && b->type->kind == TYPE_MATRIX)
                {
                    // row vector (R) * matrix (RxC) -> vector (C)
                    if (a->type->vector.size != b->type->matrix.col_count ||
                        a->type->vector.elem_type != b->type->matrix.col_type->vector.elem_type)
                    {
                        ts__addErr(compiler, &expr->loc,
                                   "mul: vector size must match the matrix row count");
                        break;
                    }
                    expr->type = b->type->matrix.col_type;
                }
                else if (a->type->kind == TYPE_MATRIX && b->type->kind == TYPE_VECTOR)
                {
                    // matrix (RxC) * column vector (C) -> vector (R)
                    if (b->type != a->type->matrix.col_type)
                    {
                        ts__addErr(compiler, &expr->loc,
                                   "mul: vector size must match the matrix column count");
                        break;
                    }
                    expr->type = newVectorType(
                        m, a->type->matrix.col_type->vector.elem_type, a->type->matrix.col_count);
                }
                else if (a->type->kind == TYPE_VECTOR && b->type->kind == TYPE_VECTOR)
                {
                    // Vector dot product
                    if (b->type != a->type)
                    {
                        ts__addErr(compiler, &expr->loc, "mismatched vector types");
                        break;
                    }
                    expr->type = a->type->vector.elem_type;
                }
                else if (a->type->kind == TYPE_MATRIX && b->type->kind == TYPE_MATRIX)
                {
                    // (RxK) * (KxC) -> (RxC)
                    AstType *ac = a->type->matrix.col_type;
                    AstType *bc = b->type->matrix.col_type;
                    if (ac->vector.size != b->type->matrix.col_count ||
                        ac->vector.elem_type != bc->vector.elem_type)
                    {
                        ts__addErr(compiler, &expr->loc, "mul: mismatched matrix dimensions");
                        break;
                    }
                    expr->type = newMatrixType(m, bc, a->type->matrix.col_count);
                }
                else
                {
                    ts__addErr(compiler, &expr->loc, "invalid parameters for mul");
                }
                break;
            }
            case AST_BUILTIN_FUNC_DISTANCE: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "distance needs 2 parameters");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];

                if ((a->type->kind != TYPE_VECTOR) || (b->type->kind != TYPE_VECTOR))
                {
                    ts__addErr(compiler, &expr->loc, "distance operates on vectors");
                    break;
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "distance cannot operate on different types");
                    break;
                }

                expr->type = ts__getElemType(a->type);
                break;
            }

            case AST_BUILTIN_FUNC_RADIANS:
            case AST_BUILTIN_FUNC_DEGREES: {
                if (param_count != 1)
                {
                    ts__addErr(
                        compiler, &expr->loc, "degrees/radians call needs 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                if (a->type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "degrees/radians call needs a float parameter");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_SIN:
            case AST_BUILTIN_FUNC_COS:
            case AST_BUILTIN_FUNC_TAN:
            case AST_BUILTIN_FUNC_ASIN:
            case AST_BUILTIN_FUNC_ACOS:
            case AST_BUILTIN_FUNC_ATAN:
            case AST_BUILTIN_FUNC_SINH:
            case AST_BUILTIN_FUNC_COSH:
            case AST_BUILTIN_FUNC_TANH: {
                if (param_count != 1)
                {
                    ts__addErr(
                        compiler, &expr->loc, "trigonometric functions take 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstType *scalar_type = ts__getScalarType(params.ptr[0]->type);

                if (scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "trigonometric functions operate on floats");
                    break;
                }

                expr->type = params.ptr[0]->type;
                break;
            }

            case AST_BUILTIN_FUNC_ATAN2: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "atan2 takes 2 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 2, true);

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];

                if (a->kind == EXPR_PRIMARY && a->type->kind == TYPE_INT)
                {
                    a->type = newFloatType(m, 32);
                }

                if (b->kind == EXPR_PRIMARY && b->type->kind == TYPE_INT)
                {
                    b->type = newFloatType(m, 32);
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "atan2 requires 2 parameters of identical types");
                    break;
                }

                if (a->type->kind != TYPE_FLOAT)
                {
                    ts__addErr(compiler, &expr->loc, "atan2 operates on floats");
                    break;
                }

                expr->type = a->type;
                break;
            }

            case AST_BUILTIN_FUNC_SQRT:
            case AST_BUILTIN_FUNC_RSQRT: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "sqrt/rsqrt takes 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "sqrt/rsqrt operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_REFLECT: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "reflect needs 2 parameters");
                    break;
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];

                if ((a->type->kind != TYPE_VECTOR) || (b->type->kind != TYPE_VECTOR))
                {
                    ts__addErr(compiler, &expr->loc, "reflect operates on vectors");
                    break;
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "reflect cannot operate on different vector types");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_REFRACT: {
                if (param_count != 3)
                {
                    ts__addErr(compiler, &expr->loc, "refract needs 2 parameters");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[2], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                AstExpr *c = params.ptr[2];

                if (!a->type || !b->type || !c->type) break;

                if ((a->type->kind != TYPE_VECTOR) || (b->type->kind != TYPE_VECTOR))
                {
                    ts__addErr(compiler, &expr->loc, "refract operates on vectors");
                    break;
                }

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "refract cannot operate on different vector types");
                    break;
                }

                if (c->type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "refract takes a scalar as the third parameter");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_POW: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "pow takes 2 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 2, true);

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                if (!a->type || !b->type) break;

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "pow operates on parameters of the same type");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "pow operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_EXP:
            case AST_BUILTIN_FUNC_EXP2: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "exp/exp2 takes 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "exp/exp2 operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_LOG:
            case AST_BUILTIN_FUNC_LOG2: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "log/log2 takes 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "log/log2 operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_SIGN:
            case AST_BUILTIN_FUNC_ABS: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "abs takes 1 parameter");
                    break;
                }

                if (expected_type)
                {
                    tryCoerceExprToScalarType(a, params.ptr[0], expected_type);
                }

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type ||
                    !(scalar_type->kind == TYPE_INT || scalar_type->kind == TYPE_FLOAT))
                {
                    ts__addErr(
                        compiler, &expr->loc, "abs/sign operate on vectors or scalars");
                    break;
                }

                expr->type = a->type;

                if (builtin_func_kind == AST_BUILTIN_FUNC_SIGN &&
                    scalar_type->kind == TYPE_FLOAT)
                {
                    // HLSL sign() returns int: FSign, then convert
                    AstType *int_type = newIntType(m, 32, true);
                    AstType *res_type = (a->type->kind == TYPE_VECTOR)
                        ? newVectorType(m, int_type, a->type->vector.size) : int_type;
                    // 'a' is shadowed by the parameter here; use the analyzer via the expr
                    wrapAutoCastM(compiler, expr, res_type);
                }

                break;
            }

            case AST_BUILTIN_FUNC_MIN:
            case AST_BUILTIN_FUNC_MAX: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "min/max takes 2 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 2, false);

                if (expected_type &&
                    canCoerceExprToScalarType(a, params.ptr[0], expected_type) &&
                    canCoerceExprToScalarType(a, params.ptr[1], expected_type))
                {
                    tryCoerceExprToScalarType(a, params.ptr[0], expected_type);
                    tryCoerceExprToScalarType(a, params.ptr[1], expected_type);
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                if (!a->type || !b->type) break;

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "min/max operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type ||
                    !(scalar_type->kind == TYPE_INT || scalar_type->kind == TYPE_FLOAT))
                {
                    ts__addErr(
                        compiler, &expr->loc, "min/max operates on vectors or scalars");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_TRUNC:
            case AST_BUILTIN_FUNC_CEIL:
            case AST_BUILTIN_FUNC_FLOOR:
            case AST_BUILTIN_FUNC_ROUND:
            case AST_BUILTIN_FUNC_FRAC: {
                if (param_count != 1)
                {
                    ts__addErr(
                        compiler, &expr->loc, "frac/ceil/trunc/floor take 1 parameter");
                    break;
                }

                if (expected_type &&
                    canCoerceExprToScalarType(a, params.ptr[0], expected_type))
                {
                    tryCoerceExprToScalarType(a, params.ptr[0], expected_type);
                }

                if (!params.ptr[0]->type) break;

                AstType *scalar_type = ts__getScalarType(params.ptr[0]->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "frac/ceil/trunc/floor operates on vectors or scalars of "
                        "floating point types");
                    break;
                }

                expr->type = params.ptr[0]->type;

                break;
            }

            case AST_BUILTIN_FUNC_LERP: {
                if (param_count != 3)
                {
                    ts__addErr(compiler, &expr->loc, "lerp takes 3 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 3, true);

                if (expected_type &&
                    canCoerceExprToScalarType(a, params.ptr[0], expected_type) &&
                    canCoerceExprToScalarType(a, params.ptr[1], expected_type) &&
                    canCoerceExprToScalarType(a, params.ptr[2], expected_type))
                {
                    tryCoerceExprToScalarType(a, params.ptr[0], expected_type);
                    tryCoerceExprToScalarType(a, params.ptr[1], expected_type);
                    tryCoerceExprToScalarType(a, params.ptr[2], expected_type);
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                AstExpr *c = params.ptr[2];
                if (!a->type || !b->type || !c->type) break;

                if (a->type != b->type || a->type != c->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "lerp operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "lerp operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_CLAMP: {
                if (param_count != 3)
                {
                    ts__addErr(compiler, &expr->loc, "clamp takes 3 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 3, false);

                if (expected_type &&
                    canCoerceExprToScalarType(a, params.ptr[0], expected_type) &&
                    canCoerceExprToScalarType(a, params.ptr[1], expected_type) &&
                    canCoerceExprToScalarType(a, params.ptr[2], expected_type))
                {
                    tryCoerceExprToScalarType(a, params.ptr[0], expected_type);
                    tryCoerceExprToScalarType(a, params.ptr[1], expected_type);
                    tryCoerceExprToScalarType(a, params.ptr[2], expected_type);
                }
                else if (
                    canCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type) &&
                    canCoerceExprToScalarType(a, params.ptr[2], params.ptr[0]->type))
                {
                    tryCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type);
                    tryCoerceExprToScalarType(a, params.ptr[2], params.ptr[0]->type);
                }

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                AstExpr *c = params.ptr[2];
                if (!a->type || !b->type || !c->type) break;

                if (a->type != b->type || a->type != c->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "clamp operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type ||
                    !(scalar_type->kind == TYPE_FLOAT || scalar_type->kind == TYPE_INT))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "clamp operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_STEP: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "step takes 2 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 2, true);

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                if (!a->type || !b->type) break;

                if (a->type != b->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "step operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "step operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_SMOOTHSTEP: {
                if (param_count != 3)
                {
                    ts__addErr(compiler, &expr->loc, "smoothstep takes 3 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 3, true);

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[2], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                AstExpr *b = params.ptr[1];
                AstExpr *c = params.ptr[2];
                if (!a->type || !b->type || !c->type) break;

                if ((a->type != b->type) || (a->type != c->type))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "smoothstep operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "smoothstep operates on vectors or scalars of float type");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_FMOD: {
                if (param_count != 2)
                {
                    ts__addErr(compiler, &expr->loc, "fmod needs 2 parameters");
                    break;
                }

                unifyIntrinsicArgs(a, params.ptr, 2, true);

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));
                tryCoerceExprToScalarType(a, params.ptr[1], newFloatType(m, 32));

                if (!params.ptr[0]->type || !params.ptr[1]->type) break;

                if (params.ptr[0]->type != params.ptr[1]->type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "fmod operates parameters of equal types");
                    break;
                }

                AstType *scalar_type = ts__getScalarType(params.ptr[0]->type);
                if (!scalar_type || scalar_type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "fmod operates on vectors or scalars of float type");
                    break;
                }

                expr->type = params.ptr[0]->type;
                break;
            }

            case AST_BUILTIN_FUNC_TRANSPOSE: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "transpose takes 1 parameter");
                    break;
                }

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                if (a->type->kind != TYPE_MATRIX)
                {
                    ts__addErr(compiler, &expr->loc, "transpose operates on matrices");
                    break;
                }

                AstType *elem_type = a->type->matrix.col_type->vector.elem_type;
                uint32_t col_count = a->type->matrix.col_count;
                uint32_t row_count = a->type->matrix.col_type->vector.size;

                AstType *col_type = newVectorType(m, elem_type, col_count);
                expr->type = newMatrixType(m, col_type, row_count);

                break;
            }

            case AST_BUILTIN_FUNC_DETERMINANT: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "determinant takes 1 parameter");
                    break;
                }

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                if (a->type->kind != TYPE_MATRIX)
                {
                    ts__addErr(compiler, &expr->loc, "determinant operates on matrices");
                    break;
                }

                AstType *elem_type = a->type->matrix.col_type->vector.elem_type;
                uint32_t col_count = a->type->matrix.col_count;
                uint32_t row_count = a->type->matrix.col_type->vector.size;

                expr->type = elem_type;

                if (col_count != row_count)
                {
                    ts__addErr(
                        compiler, &expr->loc, "determinant operates on square matrices");
                    break;
                }

                break;
            }

            case AST_BUILTIN_FUNC_DDX:
            case AST_BUILTIN_FUNC_DDY: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "ddx/ddy call needs 1 parameter");
                    break;
                }

                tryCoerceExprToScalarType(a, params.ptr[0], newFloatType(m, 32));

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                if (a->type->kind != TYPE_FLOAT)
                {
                    ts__addErr(
                        compiler, &expr->loc, "ddx/ddy call needs a float parameter");
                    break;
                }

                expr->type = a->type;

                break;
            }

            case AST_BUILTIN_FUNC_ASFLOAT:
            case AST_BUILTIN_FUNC_ASINT:
            case AST_BUILTIN_FUNC_ASUINT: {
                if (param_count != 1)
                {
                    ts__addErr(compiler, &expr->loc, "bitcast needs 1 parameter");
                    break;
                }

                AstExpr *a = params.ptr[0];
                if (!a->type) break;

                uint32_t dim = 0;
                if (a->type->kind == TYPE_VECTOR)
                {
                    dim = a->type->vector.size;
                }

                AstType *scalar_type = ts__getScalarType(a->type);
                if (!scalar_type)
                {
                    ts__addErr(
                        compiler, &expr->loc, "bitcast needs a scalar/vector parameter");
                    break;
                }

                switch (builtin_func_kind)
                {
                case AST_BUILTIN_FUNC_ASFLOAT: {
                    expr->type = newFloatType(m, 32);
                    break;
                }
                case AST_BUILTIN_FUNC_ASINT: {
                    expr->type = newIntType(m, 32, true);
                    break;
                }
                case AST_BUILTIN_FUNC_ASUINT: {
                    expr->type = newIntType(m, 32, false);
                    break;
                }
                default: TS_ASSERT(0); break;
                }

                if (dim > 0)
                {
                    expr->type = newVectorType(m, expr->type, dim);
                }

                break;
            }

            case AST_BUILTIN_FUNC_INTERLOCKED_OR:
            case AST_BUILTIN_FUNC_INTERLOCKED_XOR:
            case AST_BUILTIN_FUNC_INTERLOCKED_MIN:
            case AST_BUILTIN_FUNC_INTERLOCKED_MAX:
            case AST_BUILTIN_FUNC_INTERLOCKED_AND:
            case AST_BUILTIN_FUNC_INTERLOCKED_ADD: {
                expr->type = newBasicType(m, TYPE_VOID);

                if (param_count != 2)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires 2 parameters");
                    break;
                }

                if (!params.ptr[0]->type || !params.ptr[1]->type) break;

                tryCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type);

                if (params.ptr[0]->type != params.ptr[1]->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "mismatched types for Interlocked function parameters");
                    break;
                }

                if (params.ptr[0]->type->kind != TYPE_INT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires scalar integer parameters");
                    break;
                }

                if ((params.ptr[0]->kind != EXPR_IDENT) || (!params.ptr[0]->ident.decl) ||
                    (params.ptr[0]->ident.decl->kind != DECL_VAR) ||
                    (params.ptr[0]->ident.decl->var.storage_class != VAR_STORAGE_CLASS_GROUPSHARED))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked functions require first parameter to be a "
                        "groupshared variable");
                    break;
                }

                break;
            }

            case AST_BUILTIN_FUNC_INTERLOCKED_EXCHANGE: {
                expr->type = newBasicType(m, TYPE_VOID);

                if (param_count != 3)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires 3 parameters");
                    break;
                }

                if (!params.ptr[0]->type || !params.ptr[1]->type || !params.ptr[2]->type)
                    break;

                tryCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type);

                if (params.ptr[0]->type != params.ptr[1]->type ||
                    params.ptr[0]->type != params.ptr[2]->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "mismatched types for Interlocked function parameters");
                    break;
                }

                if (params.ptr[0]->type->kind != TYPE_INT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires scalar integer parameters");
                    break;
                }

                if ((params.ptr[0]->kind != EXPR_IDENT) || (!params.ptr[0]->ident.decl) ||
                    (params.ptr[0]->ident.decl->kind != DECL_VAR) ||
                    (params.ptr[0]->ident.decl->var.storage_class != VAR_STORAGE_CLASS_GROUPSHARED))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked functions require first parameter to be a "
                        "groupshared "
                        "variable");
                    break;
                }

                if (!params.ptr[2]->assignable)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires third parameter to be assignable");
                    break;
                }

                break;
            }

            case AST_BUILTIN_FUNC_INTERLOCKED_COMPARE_EXCHANGE: {
                expr->type = newBasicType(m, TYPE_VOID);

                if (param_count != 4)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires 4 parameters");
                    break;
                }

                if (!params.ptr[0]->type || !params.ptr[1]->type ||
                    !params.ptr[2]->type || !params.ptr[3]->type)
                    break;

                tryCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type);
                tryCoerceExprToScalarType(a, params.ptr[2], params.ptr[0]->type);

                if (params.ptr[0]->type != params.ptr[1]->type ||
                    params.ptr[0]->type != params.ptr[2]->type ||
                    params.ptr[0]->type != params.ptr[3]->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "mismatched types for Interlocked function parameters");
                    break;
                }

                if (params.ptr[0]->type->kind != TYPE_INT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires scalar integer parameters");
                    break;
                }

                if ((params.ptr[0]->kind != EXPR_IDENT) || (!params.ptr[0]->ident.decl) ||
                    (params.ptr[0]->ident.decl->kind != DECL_VAR) ||
                    (params.ptr[0]->ident.decl->var.storage_class != VAR_STORAGE_CLASS_GROUPSHARED))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked functions require first parameter to be a "
                        "groupshared "
                        "variable");
                    break;
                }

                if (!params.ptr[3]->assignable)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires fourth parameter to be "
                        "assignable");
                    break;
                }

                break;
            }

            case AST_BUILTIN_FUNC_INTERLOCKED_COMPARE_STORE: {
                expr->type = newBasicType(m, TYPE_VOID);

                if (param_count != 3)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires 3 parameters");
                    break;
                }

                if (!params.ptr[0]->type || !params.ptr[1]->type || !params.ptr[2]->type)
                    break;

                tryCoerceExprToScalarType(a, params.ptr[1], params.ptr[0]->type);
                tryCoerceExprToScalarType(a, params.ptr[2], params.ptr[0]->type);

                if (params.ptr[0]->type != params.ptr[1]->type ||
                    params.ptr[0]->type != params.ptr[2]->type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "mismatched types for Interlocked function parameters");
                    break;
                }

                if (params.ptr[0]->type->kind != TYPE_INT)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked function requires scalar integer parameters");
                    break;
                }

                if ((params.ptr[0]->kind != EXPR_IDENT) || (!params.ptr[0]->ident.decl) ||
                    (params.ptr[0]->ident.decl->kind != DECL_VAR) ||
                    (params.ptr[0]->ident.decl->var.storage_class != VAR_STORAGE_CLASS_GROUPSHARED))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "Interlocked functions require first parameter to be a "
                        "groupshared "
                        "variable");
                    break;
                }

                break;
            }

            case AST_BUILTIN_FUNC_ALL_MEMORY_BARRIER:
            case AST_BUILTIN_FUNC_ALL_MEMORY_BARRIER_WITH_GROUP_SYNC:
            case AST_BUILTIN_FUNC_DEVICE_MEMORY_BARRIER:
            case AST_BUILTIN_FUNC_DEVICE_MEMORY_BARRIER_WITH_GROUP_SYNC:
            case AST_BUILTIN_FUNC_GROUP_MEMORY_BARRIER:
            case AST_BUILTIN_FUNC_GROUP_MEMORY_BARRIER_WITH_GROUP_SYNC: {
                expr->type = newBasicType(m, TYPE_VOID);

                if (param_count != 0)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "barrier functions require 0 parameters");
                    break;
                }

                break;
            }
            }

            break;
        }

        // Builtin method call
        if (func_expr->kind == EXPR_ACCESS)
        {
            AstExpr *method_name_expr =
                func_expr->access.chain.ptr[arrLength(func_expr->access.chain) - 1];
            TS_ASSERT(method_name_expr->kind == EXPR_IDENT);
            char *method_name = method_name_expr->ident.name;

            // Remove last element from access (the method name)
            arrPop(&func_expr->access.chain);

            if (arrLength(func_expr->access.chain) == 0)
            {
                func_expr = func_expr->access.base;
            }

            expr->func_call.self_param = func_expr;
            expr->func_call.func_expr = method_name_expr;

            analyzerAnalyzeExpr(a, expr->func_call.self_param, NULL);
            AstType *self_type = expr->func_call.self_param->type;
            if (!self_type)
            {
                break;
            }

            if (self_type->kind == TYPE_IMAGE && strcmp(method_name, "Sample") == 0)
            {
                AstType *texture_component_type = self_type->image.sampled_type;

                uint32_t func_param_count = 2;
                AstType **func_param_types =
                    NEW_ARRAY(compiler, AstType *, func_param_count);

                func_param_types[0] = newBasicType(m, TYPE_SAMPLER);

                AstType *float_type = newFloatType(m, 32);
                switch (self_type->image.dim)
                {
                case SpvDim1D:
                    func_param_types[1] = newVectorType(m, float_type, 1);
                    break;
                case SpvDim2D:
                    func_param_types[1] = newVectorType(m, float_type, 2);
                    break;
                case SpvDim3D:
                case SpvDimCube:
                    func_param_types[1] = newVectorType(m, float_type, 3);
                    break;

                default: TS_ASSERT(0); break;
                }

                if (func_param_count != arrLength(expr->func_call.params))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "wrong amount of parameters for function call");
                    break;
                }

                for (uint32_t i = 0; i < arrLength(expr->func_call.params); ++i)
                {
                    AstExpr *param = expr->func_call.params.ptr[i];
                    analyzerAnalyzeExpr(a, param, func_param_types[i]);
                }

                expr->type = texture_component_type;
            }
            else if (
                self_type->kind == TYPE_IMAGE && strcmp(method_name, "SampleLevel") == 0)
            {
                AstType *texture_component_type = self_type->image.sampled_type;

                uint32_t func_param_count = 3;
                AstType **func_param_types =
                    NEW_ARRAY(compiler, AstType *, func_param_count);

                func_param_types[0] = newBasicType(m, TYPE_SAMPLER);

                AstType *float_type = newFloatType(m, 32);
                func_param_types[2] = float_type;

                switch (self_type->image.dim)
                {
                case SpvDim1D:
                    func_param_types[1] = newVectorType(m, float_type, 1);
                    break;
                case SpvDim2D:
                    func_param_types[1] = newVectorType(m, float_type, 2);
                    break;
                case SpvDim3D:
                case SpvDimCube:
                    func_param_types[1] = newVectorType(m, float_type, 3);
                    break;

                default: TS_ASSERT(0); break;
                }

                if (func_param_count != arrLength(expr->func_call.params))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "wrong amount of parameters for function call");
                    break;
                }

                for (uint32_t i = 0; i < arrLength(expr->func_call.params); ++i)
                {
                    AstExpr *param = expr->func_call.params.ptr[i];
                    analyzerAnalyzeExpr(a, param, func_param_types[i]);
                }

                expr->type = texture_component_type;
            }
            else if (
                self_type->kind == TYPE_IMAGE &&
                strcmp(method_name, "GetDimensions") == 0)
            {
                uint32_t func_param_count = 0;
                AstType **func_param_types = NULL;

                AstType *uint_type = newIntType(m, 32, false);
                AstType *float_type = newFloatType(m, 32);

                switch (self_type->image.dim)
                {
                case SpvDim1D:
                    func_param_count = 3;
                    func_param_types = NEW_ARRAY(compiler, AstType *, func_param_count);

                    func_param_types[0] = uint_type;  // mip level
                    func_param_types[1] = float_type; // width
                    func_param_types[2] = float_type; // mip count
                    break;
                case SpvDim2D:
                    func_param_count = 4;
                    func_param_types = NEW_ARRAY(compiler, AstType *, func_param_count);

                    func_param_types[0] = uint_type;  // mip level
                    func_param_types[1] = float_type; // width
                    func_param_types[2] = float_type; // height
                    func_param_types[3] = float_type; // mip count
                    break;
                case SpvDim3D:
                    func_param_count = 5;
                    func_param_types = NEW_ARRAY(compiler, AstType *, func_param_count);

                    func_param_types[0] = uint_type;  // mip level
                    func_param_types[1] = float_type; // width
                    func_param_types[2] = float_type; // height
                    func_param_types[3] = float_type; // depth
                    func_param_types[4] = float_type; // mip count
                    break;
                case SpvDimCube:
                    func_param_count = 4;
                    func_param_types = NEW_ARRAY(compiler, AstType *, func_param_count);

                    func_param_types[0] = uint_type;  // mip level
                    func_param_types[1] = float_type; // width
                    func_param_types[2] = float_type; // height
                    func_param_types[3] = float_type; // mip count
                    break;

                default: TS_ASSERT(0); break;
                }

                TS_ASSERT(func_param_count > 0);

                if (func_param_count != arrLength(expr->func_call.params))
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "wrong amount of parameters for function call");
                    break;
                }

                for (uint32_t i = 0; i < arrLength(expr->func_call.params); ++i)
                {
                    AstExpr *param = expr->func_call.params.ptr[i];
                    analyzerAnalyzeExpr(a, param, func_param_types[i]);
                }

                expr->type = newBasicType(m, TYPE_VOID);
            }
            else
            {
                ts__addErr(compiler, &expr->loc, "invalid method call");
            }

            break;
        }

        analyzerAnalyzeExpr(a, func_expr, NULL);
        AstType *func_type = func_expr->type;
        if (!func_type)
        {
            break;
        }

        if (func_type->kind == TYPE_TYPE)
        {
            // Type constructor

            AstType *constructed_type = func_expr->as_type;
            TS_ASSERT(constructed_type);

            if (expr->func_call.is_cast)
            {
                analyzerAnalyzeCast(a, expr, constructed_type);
                break;
            }

            expr->type = constructed_type;

            uint32_t param_count = arrLength(expr->func_call.params);
            ArrayOfAstExprPtr params = expr->func_call.params;

            AstType *wanted_elem_type = NULL;
            uint32_t wanted_elem_count = 0;

            switch (constructed_type->kind)
            {
            case TYPE_VECTOR: {
                wanted_elem_count = constructed_type->vector.size;
                wanted_elem_type = constructed_type->vector.elem_type;

                if (!a->scope_func)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "composite constructor needs to be inside a function");
                    break;
                }

                uint32_t elem_count = 0;

                for (uint32_t i = 0; i < param_count; ++i)
                {
                    analyzerAnalyzeExpr(a, params.ptr[i], NULL);
                    tryCoerceExprToScalarType(a, params.ptr[i], wanted_elem_type);
                    if (!params.ptr[i]->type) continue;

                    // Scalars convert implicitly to the element type, as in HLSL
                    AstType *param_type = params.ptr[i]->type;
                    if (param_type->kind != TYPE_VECTOR && param_type != wanted_elem_type &&
                        isTypeCastable(param_type, wanted_elem_type))
                    {
                        wrapAutoCast(a, params.ptr[i], wanted_elem_type);
                    }
                    else if (param_type->kind == TYPE_VECTOR &&
                             param_type->vector.elem_type != wanted_elem_type &&
                             isTypeCastable(param_type->vector.elem_type, wanted_elem_type))
                    {
                        wrapAutoCast(a, params.ptr[i],
                                     newVectorType(m, wanted_elem_type, param_type->vector.size));
                    }

                    if (ts__getElemType(params.ptr[i]->type) != wanted_elem_type)
                    {
                        ts__addErr(
                            compiler,
                            &params.ptr[i]->loc,
                            "invalid composite constructor element type");
                    }

                    elem_count += ts__getTypeElemCount(params.ptr[i]->type);
                }

                if (elem_count != wanted_elem_count)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid composite constructor element count");
                    break;
                }

                break;
            }
            case TYPE_MATRIX: {
                AstType *col_type = constructed_type->matrix.col_type;
                TS_ASSERT(col_type->kind == TYPE_VECTOR);
                wanted_elem_count =
                    constructed_type->matrix.col_count * col_type->vector.size;
                wanted_elem_type = col_type->vector.elem_type;

                if (!a->scope_func)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "composite constructor needs to be inside a function");
                    break;
                }

                if (param_count != wanted_elem_count)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid composite constructor parameter count");
                    break;
                }

                for (uint32_t i = 0; i < param_count; ++i)
                {
                    analyzerAnalyzeExpr(a, params.ptr[i], wanted_elem_type);
                }
                break;
            }
            case TYPE_INT:
            case TYPE_FLOAT: {
                wanted_elem_type = constructed_type;
                wanted_elem_count = 1;

                if (param_count != wanted_elem_count)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid composite constructor parameter count");
                    break;
                }

                analyzerAnalyzeExpr(a, params.ptr[0], NULL);

                if (!params.ptr[0]->type)
                {
                    TS_ASSERT(compiler->errors.len > 0);
                    break;
                }

                if (!isTypeCastable(params.ptr[0]->type, wanted_elem_type))
                {
                    ts__addErr(
                        compiler,
                        &params.ptr[0]->loc,
                        "value is not castable to this type");
                    break;
                }

                break;
            }
            default: {
                ts__addErr(compiler, &expr->loc, "invalid constructor");
                break;
            }
            }
        }
        else if (func_type->kind == TYPE_FUNC)
        {
            // Actual function call

            if (func_type->func.param_count != arrLength(expr->func_call.params))
            {
                ts__addErr(
                    compiler, &expr->loc, "wrong amount of parameters for function call");
                break;
            }

            for (uint32_t i = 0; i < func_type->func.param_count; ++i)
            {
                AstExpr *param = expr->func_call.params.ptr[i];
                AstType *param_expected = func_type->func.params[i];
                if (param_expected->kind == TYPE_POINTER)
                {
                    param_expected = param_expected->ptr.sub;
                }
                analyzerAnalyzeExpr(a, param, param_expected);
            }

            expr->type = func_type->func.return_type;
        }
        else
        {
            ts__addErr(
                compiler,
                &expr->func_call.func_expr->loc,
                "expression does not represent a function");
        }

        break;
    }

    case EXPR_ARRAY_TYPE:
    {
        analyzerAnalyzeExpr(a, expr->array_type.sub, NULL);
        analyzerAnalyzeExpr(a, expr->array_type.size, NULL);

        AstType *subtype = expr->array_type.sub->as_type;
        if (!subtype)
        {
            TS_ASSERT(compiler->errors.len > 0);
            break;
        }

        if (!expr->array_type.size->resolved_int)
        {
            ts__addErr(
                compiler,
                &expr->array_type.size->loc,
                "could not resolve integer from array size expression");
            break;
        }

        if ((*expr->array_type.size->resolved_int) <= 0)
        {
            ts__addErr(
                compiler,
                &expr->array_type.size->loc,
                "array size must be greater than zero");
            break;
        }

        expr->type = newBasicType(m, TYPE_TYPE);
        expr->as_type = newArrayType(m, subtype, *expr->array_type.size->resolved_int);
        break;
    }

    case EXPR_RUNTIME_ARRAY_TYPE:
    {
        analyzerAnalyzeExpr(a, expr->array_type.sub, NULL);
        AstType *subtype = expr->array_type.sub->as_type;
        if (!subtype)
        {
            TS_ASSERT(compiler->errors.len > 0);
            break;
        }

        expr->type = newBasicType(m, TYPE_TYPE);
        expr->as_type = newRuntimeArrayType(m, subtype);
        break;
    }

    case EXPR_SAMPLER_TYPE: {
        expr->type = newBasicType(m, TYPE_TYPE);
        expr->as_type = newBasicType(m, TYPE_SAMPLER);
        break;
    }

    case EXPR_TEXTURE_TYPE: {
        AstType *type_type = newBasicType(m, TYPE_TYPE);

        AstType *sampled_type = NULL;
        if (expr->texture.sampled_type_expr)
        {
            analyzerAnalyzeExpr(a, expr->texture.sampled_type_expr, type_type);
            if (!expr->texture.sampled_type_expr->type) break;

            sampled_type = expr->texture.sampled_type_expr->as_type;
        }
        else
        {
            sampled_type = newVectorType(m, newFloatType(m, 32), 4);
        }

        TS_ASSERT(sampled_type);

        if (!(sampled_type->kind == TYPE_VECTOR || sampled_type->kind == TYPE_INT ||
              sampled_type->kind == TYPE_FLOAT))
        {
            ts__addErr(
                compiler, &expr->loc, "invalid scalar type for sampled type for texture");
            break;
        }

        expr->type = type_type;
        expr->as_type = newImageType(m, sampled_type, expr->texture.dim);
        break;
    }

    case EXPR_CONSTANT_BUFFER_TYPE: {
        TS_ASSERT(expr->buffer.sub_expr);

        AstType *type_type = newBasicType(m, TYPE_TYPE);

        analyzerAnalyzeExpr(a, expr->buffer.sub_expr, type_type);
        if (!expr->buffer.sub_expr->type) break;

        AstType *subtype = expr->buffer.sub_expr->as_type;
        TS_ASSERT(subtype);

        if (subtype->kind != TYPE_STRUCT)
        {
            ts__addErr(
                compiler,
                &expr->buffer.sub_expr->loc,
                "'%s' cannot be used as a type parameter where a struct is required",
                ts__typeToPrettyString(compiler, subtype));
            break;
        }

        expr->type = type_type;
        expr->as_type = newConstantBufferType(m, subtype);

        break;
    }

    case EXPR_STRUCTURED_BUFFER_TYPE: {
        TS_ASSERT(expr->buffer.sub_expr);

        AstType *type_type = newBasicType(m, TYPE_TYPE);

        analyzerAnalyzeExpr(a, expr->buffer.sub_expr, type_type);
        if (!expr->buffer.sub_expr->type) break;

        AstType *subtype = expr->buffer.sub_expr->as_type;
        TS_ASSERT(subtype);

        expr->type = type_type;
        expr->as_type = newStructuredBufferType(m, subtype);
        break;
    }

    case EXPR_RW_STRUCTURED_BUFFER_TYPE: {
        TS_ASSERT(expr->buffer.sub_expr);

        AstType *type_type = newBasicType(m, TYPE_TYPE);

        analyzerAnalyzeExpr(a, expr->buffer.sub_expr, type_type);
        if (!expr->buffer.sub_expr->type) break;

        AstType *subtype = expr->buffer.sub_expr->as_type;
        TS_ASSERT(subtype);

        expr->type = type_type;
        expr->as_type = newRWStructuredBufferType(m, subtype);
        break;
    }

    case EXPR_UNARY: {
        switch (expr->unary.op)
        {
        case UNOP_NEG: {
            analyzerAnalyzeExpr(a, expr->unary.right, NULL);
            if (!expr->unary.right->type) break;
            AstType *right_type = expr->unary.right->type;
            AstType *scalar_type = ts__getScalarType(right_type);

            if (!scalar_type)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "\'negation\' expression does not work on this type");
                break;
            }

            expr->type = right_type;

            break;
        }

        case UNOP_POST_INC:
        case UNOP_PRE_INC: {
            analyzerAnalyzeExpr(a, expr->unary.right, NULL);
            if (!expr->unary.right->type) break;
            AstType *right_type = expr->unary.right->type;
            AstType *scalar_type = ts__getScalarTypeNoVec(right_type);

            if (!scalar_type)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "\'increment\' expression does not work on this type");
                break;
            }

            if (!expr->unary.right->assignable)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "expression is not assignable");
            }

            expr->type = right_type;

            break;
        }

        case UNOP_POST_DEC:
        case UNOP_PRE_DEC: {
            analyzerAnalyzeExpr(a, expr->unary.right, NULL);
            if (!expr->unary.right->type) break;
            AstType *right_type = expr->unary.right->type;
            AstType *scalar_type = ts__getScalarTypeNoVec(right_type);

            if (!scalar_type)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "\'decrement\' expression does not work on this type");
                break;
            }

            if (!expr->unary.right->assignable)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "expression is not assignable");
            }

            expr->type = right_type;

            break;
        }

        case UNOP_NOT: {
            // '!' converts its operand to bool (component-wise) and negates
            analyzerAnalyzeExpr(a, expr->unary.right, NULL);
            if (!expr->unary.right->type) break;
            AstType *right_type = expr->unary.right->type;
            AstType *elem = ts__getElemType(right_type);

            if (elem->kind != TYPE_BOOL && elem->kind != TYPE_INT && elem->kind != TYPE_FLOAT)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "\'not\' expression does not work on this type");
                break;
            }

            AstType *bool_type = newBasicType(m, TYPE_BOOL);
            AstType *target = (right_type->kind == TYPE_VECTOR)
                ? newVectorType(m, bool_type, right_type->vector.size) : bool_type;
            wrapAutoCast(a, expr->unary.right, target);
            expr->type = target;

            break;
        }

        case UNOP_BITNOT: {
            analyzerAnalyzeExpr(a, expr->unary.right, NULL);
            if (!expr->unary.right->type) break;
            AstType *right_type = expr->unary.right->type;

            if (right_type->kind != TYPE_INT)
            {
                ts__addErr(
                    compiler,
                    &expr->unary.right->loc,
                    "\'bitwise not\' expression only works on integer types");
                break;
            }

            expr->type = right_type;

            break;
        }
        }

        if (expr->unary.right->resolved_int)
        {
            int64_t right_int = *expr->unary.right->resolved_int;

            expr->resolved_int = NEW(compiler, int64_t);
            switch (expr->unary.op)
            {
            case UNOP_BITNOT: *expr->resolved_int = ~right_int; break;
            case UNOP_NEG: *expr->resolved_int = -right_int; break;
            case UNOP_NOT: *expr->resolved_int = !right_int; break;
            case UNOP_POST_DEC: *expr->resolved_int = right_int; break;
            case UNOP_PRE_DEC: *expr->resolved_int = right_int - 1; break;
            case UNOP_POST_INC: *expr->resolved_int = right_int; break;
            case UNOP_PRE_INC: *expr->resolved_int = right_int + 1; break;
            }
        }

        break;
    }

    case EXPR_BINARY: {
        analyzerAnalyzeExpr(a, expr->binary.left, NULL);
        analyzerAnalyzeExpr(a, expr->binary.right, NULL);

        switch (expr->binary.op)
        {
        case BINOP_ADD:
        case BINOP_SUB:
        case BINOP_MUL:
        case BINOP_DIV:
        case BINOP_MOD: {
            if (!expr->binary.left->type || !expr->binary.right->type) break;

            tryCoerceExprToScalarType(a, expr->binary.left, expected_type);
            tryCoerceExprToScalarType(a, expr->binary.right, expected_type);

            tryCoerceExprToScalarType(a, expr->binary.left, expr->binary.right->type);
            tryCoerceExprToScalarType(a, expr->binary.right, expr->binary.left->type);

            promoteBinaryOperands(a, expr);

            AstType *left_type = expr->binary.left->type;
            AstType *right_type = expr->binary.right->type;

            if (!left_type || !right_type)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "invalid types for binary arithmentic operation");
                break;
            }

            AstType *left_scalar = ts__getScalarTypeNoVec(left_type);
            AstType *right_scalar = ts__getScalarTypeNoVec(right_type);

            if (left_type->kind == TYPE_VECTOR && right_type->kind == TYPE_VECTOR)
            {
                // Vector and vector
                left_scalar = left_type->vector.elem_type;
                right_scalar = right_type->vector.elem_type;
                if (left_scalar != right_scalar ||
                    left_type->vector.size != right_type->vector.size)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid types for binary arithmentic operation: '%s' and '%s'",
                        ts__typeToPrettyString(compiler, left_type),
                        ts__typeToPrettyString(compiler, right_type));
                    break;
                }

                expr->type = left_type;
            }
            else if (left_scalar && right_scalar)
            {
                // Scalar and scalar
                if (left_scalar != right_scalar)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid types for binary arithmentic operation: '%s' and '%s'",
                        ts__typeToPrettyString(compiler, left_type),
                        ts__typeToPrettyString(compiler, right_type));
                    break;
                }

                expr->type = left_scalar;
            }
            else
            {
                // Vector and scalar
                AstType *vector_type = NULL;
                AstType *scalar_type = NULL;
                if (left_type->kind == TYPE_VECTOR)
                {
                    TS_ASSERT(!vector_type);
                    vector_type = left_type;
                    scalar_type = right_type;
                }
                else if (right_type->kind == TYPE_VECTOR)
                {
                    TS_ASSERT(!vector_type);
                    vector_type = right_type;
                    scalar_type = left_type;
                }
                else
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid types for binary arithmentic operation: '%s' and '%s'",
                        ts__typeToPrettyString(compiler, left_type),
                        ts__typeToPrettyString(compiler, right_type));
                    break;
                }

                TS_ASSERT(vector_type);
                TS_ASSERT(scalar_type);
                TS_ASSERT(vector_type->kind == TYPE_VECTOR);
                TS_ASSERT(scalar_type->kind != TYPE_VECTOR);

                if (ts__getScalarType(vector_type) != scalar_type)
                {
                    ts__addErr(
                        compiler,
                        &expr->loc,
                        "invalid types for binary arithmentic operation");
                    break;
                }

                expr->type = vector_type;
            }

            TS_ASSERT(expr->type);

            break;
        }

        case BINOP_EQ:
        case BINOP_NOTEQ:
        case BINOP_LESS:
        case BINOP_LESSEQ:
        case BINOP_GREATER:
        case BINOP_GREATEREQ: {
            if (!expr->binary.left->type || !expr->binary.right->type) break;

            tryCoerceExprToScalarType(a, expr->binary.left, expr->binary.right->type);
            tryCoerceExprToScalarType(a, expr->binary.right, expr->binary.left->type);

            promoteBinaryOperands(a, expr);

            AstType *left_type = expr->binary.left->type;
            AstType *right_type = expr->binary.right->type;

            if (!left_type || !right_type) break;

            // Vector comparisons are component-wise and give a bool vector;
            // a scalar operand is splatted to the other side's size.
            if (left_type->kind == TYPE_VECTOR && right_type->kind != TYPE_VECTOR &&
                ts__getComparableType(right_type))
            {
                wrapAutoCast(a, expr->binary.right,
                             newVectorType(m, right_type, left_type->vector.size));
                right_type = expr->binary.right->type;
            }
            else if (right_type->kind == TYPE_VECTOR && left_type->kind != TYPE_VECTOR &&
                     ts__getComparableType(left_type))
            {
                wrapAutoCast(a, expr->binary.left,
                             newVectorType(m, left_type, right_type->vector.size));
                left_type = expr->binary.left->type;
            }

            AstType *left_comparable = ts__getComparableType(ts__getElemType(left_type));
            AstType *right_comparable = ts__getComparableType(ts__getElemType(right_type));

            if ((!left_comparable) || (!right_comparable) || (left_type != right_type))
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "invalid types for binary comparison operation");
            }

            if (left_type->kind == TYPE_VECTOR)
                expr->type = newVectorType(m, newBasicType(m, TYPE_BOOL), left_type->vector.size);
            else
                expr->type = newBasicType(m, TYPE_BOOL);

            break;
        }

        case BINOP_RSHIFT:
        case BINOP_LSHIFT: {
            if (!expr->binary.left->type || !expr->binary.right->type) break;

            tryCoerceExprToScalarType(a, expr->binary.left, expr->binary.right->type);
            tryCoerceExprToScalarType(a, expr->binary.right, expr->binary.left->type);

            AstType *left_type = expr->binary.left->type;
            AstType *right_type = expr->binary.right->type;

            if (left_type->kind != TYPE_INT || right_type->kind != TYPE_INT)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "bitwise shift requires both operands to be integers");
            }

            expr->type = left_type;

            break;
        }

        case BINOP_BITXOR:
        case BINOP_BITOR:
        case BINOP_BITAND: {
            if (!expr->binary.left->type || !expr->binary.right->type) break;

            tryCoerceExprToScalarType(a, expr->binary.left, expr->binary.right->type);
            tryCoerceExprToScalarType(a, expr->binary.right, expr->binary.left->type);

            AstType *left_type = expr->binary.left->type;
            AstType *right_type = expr->binary.right->type;

            if (left_type->kind != TYPE_INT || right_type->kind != TYPE_INT ||
                left_type != right_type)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "bitwise logical operator requires both operands to be integers of "
                    "equal type");
            }

            expr->type = left_type;

            break;
        }

        case BINOP_LOGICAL_AND:
        case BINOP_LOGICAL_OR: {
            AstType *bool_type = newBasicType(m, TYPE_BOOL);
            AstExpr *l = expr->binary.left;
            AstExpr *r = expr->binary.right;

            analyzerAnalyzeExpr(a, l, NULL);
            analyzerAnalyzeExpr(a, r, NULL);
            if (!l->type || !r->type) break;

            // and()/or() (and && / || on vectors) work component-wise
            uint32_t size = 0;
            if (l->type->kind == TYPE_VECTOR) size = l->type->vector.size;
            if (r->type->kind == TYPE_VECTOR)
            {
                if (size && size != r->type->vector.size)
                {
                    ts__addErr(compiler, &expr->loc, "logical operands have different sizes");
                    break;
                }
                size = r->type->vector.size;
            }

            AstType *target = size ? newVectorType(m, bool_type, size) : bool_type;
            AstExpr *sides[2] = {l, r};
            for (int i = 0; i < 2; ++i)
            {
                AstType *e = ts__getElemType(sides[i]->type);
                if (e->kind != TYPE_BOOL && e->kind != TYPE_INT && e->kind != TYPE_FLOAT)
                {
                    ts__addErr(compiler, &sides[i]->loc, "expected a boolean value");
                    continue;
                }
                wrapAutoCast(a, sides[i], target);
            }

            expr->type = target;
            break;
        }
        }

        if (expr->binary.left->resolved_int && expr->binary.right->resolved_int &&
            !((expr->binary.op == BINOP_DIV || expr->binary.op == BINOP_MOD) &&
              *expr->binary.right->resolved_int == 0))
        {
            int64_t left_int = *expr->binary.left->resolved_int;
            int64_t right_int = *expr->binary.right->resolved_int;

            expr->resolved_int = NEW(compiler, int64_t);
            switch (expr->binary.op)
            {
            case BINOP_ADD: *expr->resolved_int = left_int + right_int; break;
            case BINOP_SUB: *expr->resolved_int = left_int - right_int; break;
            case BINOP_MUL: *expr->resolved_int = left_int * right_int; break;
            case BINOP_DIV: *expr->resolved_int = left_int / right_int; break;
            case BINOP_MOD: *expr->resolved_int = left_int % right_int; break;
            case BINOP_RSHIFT: *expr->resolved_int = left_int >> right_int; break;
            case BINOP_LSHIFT: *expr->resolved_int = left_int << right_int; break;
            case BINOP_BITAND: *expr->resolved_int = left_int & right_int; break;
            case BINOP_BITOR: *expr->resolved_int = left_int | right_int; break;
            case BINOP_BITXOR: *expr->resolved_int = left_int ^ right_int; break;
            case BINOP_LOGICAL_AND: *expr->resolved_int = left_int && right_int; break;
            case BINOP_LOGICAL_OR: *expr->resolved_int = left_int || right_int; break;
            case BINOP_EQ: *expr->resolved_int = left_int == right_int; break;
            case BINOP_NOTEQ: *expr->resolved_int = left_int != right_int; break;
            case BINOP_LESS: *expr->resolved_int = left_int < right_int; break;
            case BINOP_LESSEQ: *expr->resolved_int = left_int <= right_int; break;
            case BINOP_GREATER: *expr->resolved_int = left_int > right_int; break;
            case BINOP_GREATEREQ: *expr->resolved_int = left_int >= right_int; break;
            }
        }

        break;
    }

    case EXPR_TERNARY: {
        AstExpr *cond = expr->ternary.cond;
        analyzerAnalyzeExpr(a, cond, NULL);
        if (!cond->type) break;

        // A vector condition selects component-wise (also used by select()).
        uint32_t cond_size = (cond->type->kind == TYPE_VECTOR) ? cond->type->vector.size : 0;
        AstType *bool_type = newBasicType(m, TYPE_BOOL);
        AstType *cond_target = cond_size ? newVectorType(m, bool_type, cond_size) : bool_type;
        if (cond->type != cond_target)
        {
            AstType *ce = ts__getElemType(cond->type);
            if (ce->kind != TYPE_FLOAT && ce->kind != TYPE_INT && ce->kind != TYPE_BOOL)
            {
                ts__addErr(compiler, &cond->loc, "condition must be a scalar or vector");
                break;
            }
            wrapAutoCast(a, cond, cond_target);
        }

        AstType *want = expected_type;
        analyzerAnalyzeExpr(a, expr->ternary.true_expr, want);
        AstType *true_type = expr->ternary.true_expr->type;
        if (!true_type) break;

        if (cond_size && true_type->kind != TYPE_VECTOR)
        {
            // select(mask3, 1.0, 0.0): splat the scalar branches
            AstType *vec = newVectorType(m, true_type, cond_size);
            wrapAutoCast(a, expr->ternary.true_expr, vec);
            true_type = vec;
        }

        analyzerAnalyzeExpr(a, expr->ternary.false_expr, true_type);

        if (cond_size && true_type->kind == TYPE_VECTOR && true_type->vector.size != cond_size)
        {
            ts__addErr(compiler, &cond->loc, "condition size doesn't match the result size");
        }

        expr->type = true_type;
        break;
    }

    case EXPR_COMPOSITE_LITERAL:
    {
        if (!expected_type)
        {
            ts__addErr(compiler, &expr->loc, "could not deduce type of composite literal");
            break;
        }

        AstExpr **exprs = expr->composite_literal.exprs.ptr;
        size_t exprs_length = expr->composite_literal.exprs.len;

        switch (expected_type->kind)
        {
        case TYPE_RUNTIME_ARRAY:
        {
            if (exprs_length <= 0)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "array size must be greater than zero");
                break;
            }

            expr->type = newArrayType(m, expected_type->array.sub, exprs_length);
            
            AstType *subtype = expr->type->array.sub;
            for (size_t i = 0; i < exprs_length; ++i)
            {
                analyzerAnalyzeExpr(a, exprs[i], subtype);
            }

            break;
        }
        case TYPE_ARRAY:
        {
            expr->type = expected_type;

            if (exprs_length != expr->type->array.size)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "expected %zu array elements, instead got %zu",
                    (size_t)expr->type->array.size,
                    exprs_length);
                break;
            }

            AstType *subtype = expr->type->array.sub;
            for (size_t i = 0; i < exprs_length; ++i)
            {
                analyzerAnalyzeExpr(a, exprs[i], subtype);
            }

            break;
        }
        case TYPE_VECTOR:
        {
            expr->type = expected_type;

            if (exprs_length != expr->type->vector.size)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "expected %zu vector elements, instead got %zu",
                    (size_t)expr->type->vector.size,
                    exprs_length);
                break;
            }

            AstType *subtype = expr->type->vector.elem_type;
            for (size_t i = 0; i < exprs_length; ++i)
            {
                analyzerAnalyzeExpr(a, exprs[i], subtype);
            }

            break;
        }
        case TYPE_INT:
        case TYPE_FLOAT:
        {
            expr->type = expected_type;

            if (exprs_length != 1)
            {
                ts__addErr(
                    compiler,
                    &expr->loc,
                    "expected a single value for scalar initializer, instead got %zu",
                    exprs_length);
                break;
            }

            analyzerAnalyzeExpr(a, exprs[0], expr->type);

            break;
        }
        default:
        {
            ts__addErr(
                compiler,
                &expr->loc,
                "invalid type for composite literal: '%s', expected an array or vector type",
                ts__typeToPrettyString(compiler, expected_type));
            break;
        }
        }

        break;
    }

    case EXPR_AUTO_CAST:
    {
        TS_ASSERT(expr->auto_cast.sub);
        TS_ASSERT(expr->auto_cast.sub->type);
        break;
    }
    }

    if (expected_type)
    {
        if (!expr->type)
        {
            TS_ASSERT(compiler->errors.len > 0);
            /* ts__addErr(compiler, &expr->loc, "could not resolve type for expression"); */
        }
        else if (expr->type != expected_type &&
                 !(expr->type->kind == TYPE_ARRAY &&
                   expected_type->kind == TYPE_RUNTIME_ARRAY &&
                   expr->type->array.sub == expected_type->array.sub))
        {
            bool is_auto_castable = false;

            // Cast scalar to vector
            is_auto_castable |= (expected_type->kind == TYPE_VECTOR &&
                                 ts__getScalarTypeNoVec(expr->type));

            // Cast scalar to scalar
            is_auto_castable |= (ts__getScalarTypeNoVec(expected_type) &&
                                 ts__getScalarTypeNoVec(expr->type));

            // Cast vector to vector
            is_auto_castable |= (expected_type->kind == TYPE_VECTOR &&
                                 expr->type->kind == TYPE_VECTOR &&
                                 expected_type->vector.size == expr->type->vector.size);

            if (is_auto_castable)
            {
                AstExpr *sub_expr = NEW(compiler, AstExpr);
                *sub_expr = *expr;

                expr->kind = EXPR_AUTO_CAST;
                expr->auto_cast.sub = sub_expr;
                expr->type = expected_type;
            }
            else
            {
                ts__addErr(compiler, &expr->loc,
                        "unmatched types, expected '%s', instead got '%s'",
                        ts__typeToPrettyString(compiler, expected_type),
                        ts__typeToPrettyString(compiler, expr->type));
            }
        }
    }
}

static void analyzerAnalyzeStmt(Analyzer *a, AstStmt *stmt)
{
    TsCompiler *compiler = a->compiler;

    switch (stmt->kind)
    {
    case STMT_DECL: {
        analyzerTryRegisterDecl(a, stmt->decl);
        analyzerAnalyzeDecl(a, stmt->decl);
        break;
    }

    case STMT_EXPR: {
        analyzerAnalyzeExpr(a, stmt->expr, NULL);
        break;
    }

    case STMT_RETURN: {
        TS_ASSERT(a->scope_func);
        if (!a->scope_func->type) break;

        AstType *return_type = a->scope_func->type->func.return_type;

        if (return_type->kind == TYPE_VOID && stmt->return_.value)
        {
            ts__addErr(compiler, &stmt->loc, "function does not return a value");
        }

        if (return_type->kind != TYPE_VOID && !stmt->return_.value)
        {
            ts__addErr(compiler, &stmt->loc, "function needs a return value");
        }

        if (stmt->return_.value)
        {
            analyzerAnalyzeExpr(a, stmt->return_.value, return_type);
        }
        break;
    }

    case STMT_DISCARD: {
        TS_ASSERT(a->scope_func);
        break;
    }

    case STMT_CONTINUE: {
        TS_ASSERT(a->scope_func);
        if (arrLength(a->continue_stack) == 0)
        {
            ts__addErr(
                compiler, &stmt->loc, "continue must be inside a control flow structure");
        }
        break;
    }

    case STMT_BREAK: {
        TS_ASSERT(a->scope_func);
        if (arrLength(a->break_stack) == 0)
        {
            ts__addErr(
                compiler, &stmt->loc, "break must be inside a control flow structure");
        }
        break;
    }

    case STMT_BLOCK: {
        Scope *scope = analyzerCurrentScope(a);

        stmt->block.scope = NEW(compiler, Scope);
        scopeInit(compiler, stmt->block.scope, scope, NULL);

        analyzerPushScope(a, stmt->block.scope);
        for (uint32_t i = 0; i < arrLength(stmt->block.stmts); ++i)
        {
            AstStmt *sub_stmt = stmt->block.stmts.ptr[i];
            analyzerAnalyzeStmt(a, sub_stmt);
        }
        analyzerPopScope(a, stmt->block.scope);
        break;
    }

    case STMT_IF: {
        analyzerAnalyzeExpr(a, stmt->if_.cond, NULL);
        if (stmt->if_.cond->type && !ts__getComparableType(stmt->if_.cond->type))
        {
            ts__addErr(compiler, &stmt->if_.cond->loc, "expression is not comparable");
        }

        analyzerAnalyzeStmt(a, stmt->if_.if_stmt);
        if (stmt->if_.else_stmt)
        {
            analyzerAnalyzeStmt(a, stmt->if_.else_stmt);
        }

        break;
    }

    case STMT_SWITCH: {
        AstExpr *sel = stmt->switch_.selector;
        analyzerAnalyzeExpr(a, sel, NULL);
        if (sel->type && sel->type->kind != TYPE_INT)
        {
            // HLSL allows any scalar selector convertible to int
            if (ts__getScalarTypeNoVec(sel->type))
            {
                wrapAutoCast(a, sel, newIntType(a->module, 32, true));
            }
            else
            {
                ts__addErr(compiler, &sel->loc, "switch selector must be a scalar integer");
            }
        }

        bool seen_default = false;
        for (uint32_t i = 0; i < arrLength(stmt->switch_.labels); ++i)
        {
            AstSwitchLabel *label = &stmt->switch_.labels.ptr[i];
            if (!label->value)
            {
                if (seen_default)
                {
                    ts__addErr(compiler, &stmt->loc, "multiple default labels in switch");
                }
                seen_default = true;
                continue;
            }

            analyzerAnalyzeExpr(a, label->value, NULL);
            if (!label->value->resolved_int)
            {
                ts__addErr(
                    compiler, &label->value->loc, "case label must be an integer constant");
                continue;
            }

            for (uint32_t j = 0; j < i; ++j)
            {
                AstSwitchLabel *other = &stmt->switch_.labels.ptr[j];
                if (other->value && other->value->resolved_int &&
                    *other->value->resolved_int == *label->value->resolved_int)
                {
                    ts__addErr(compiler, &label->value->loc, "duplicate case label");
                }
            }
        }

        if (arrLength(stmt->switch_.body->block.stmts) > 0 &&
            (arrLength(stmt->switch_.labels) == 0 ||
             stmt->switch_.labels.ptr[0].stmt_index > 0))
        {
            ts__addErr(compiler, &stmt->loc, "statement in switch before any case label");
        }

        arrPush(a->compiler, &a->break_stack, stmt);
        analyzerAnalyzeStmt(a, stmt->switch_.body);
        arrPop(&a->break_stack);
        break;
    }

    case STMT_WHILE: {
        analyzerAnalyzeExpr(a, stmt->while_.cond, NULL);
        if (stmt->while_.cond->type && !ts__getComparableType(stmt->while_.cond->type))
        {
            ts__addErr(compiler, &stmt->while_.cond->loc, "expression is not comparable");
        }

        arrPush(a->compiler, &a->continue_stack, stmt);
        arrPush(a->compiler, &a->break_stack, stmt);
        analyzerAnalyzeStmt(a, stmt->while_.stmt);
        arrPop(&a->continue_stack);
        arrPop(&a->break_stack);
        break;
    }

    case STMT_DO_WHILE: {
        analyzerAnalyzeExpr(a, stmt->do_while.cond, NULL);
        if (stmt->do_while.cond->type &&
            !ts__getComparableType(stmt->do_while.cond->type))
        {
            ts__addErr(
                compiler, &stmt->do_while.cond->loc, "expression is not comparable");
        }

        arrPush(a->compiler, &a->continue_stack, stmt);
        arrPush(a->compiler, &a->break_stack, stmt);
        analyzerAnalyzeStmt(a, stmt->do_while.stmt);
        arrPop(&a->continue_stack);
        arrPop(&a->break_stack);
        break;
    }

    case STMT_FOR: {
        if (stmt->for_.init)
        {
            analyzerAnalyzeStmt(a, stmt->for_.init);
        }

        if (stmt->for_.cond)
        {
            analyzerAnalyzeExpr(a, stmt->for_.cond, NULL);
            if (stmt->for_.cond->type && !ts__getComparableType(stmt->for_.cond->type))
            {
                ts__addErr(
                    compiler, &stmt->for_.cond->loc, "expression is not comparable");
            }
        }

        if (stmt->for_.inc)
        {
            analyzerAnalyzeExpr(a, stmt->for_.inc, NULL);
        }

        arrPush(a->compiler, &a->continue_stack, stmt);
        arrPush(a->compiler, &a->break_stack, stmt);
        analyzerAnalyzeStmt(a, stmt->for_.stmt);
        arrPop(&a->continue_stack);
        arrPop(&a->break_stack);
        break;
    }
    }
}

static void analyzerAnalyzeDecl(Analyzer *a, AstDecl *decl)
{
    TsCompiler *compiler = a->compiler;
    Module *m = a->module;
    Scope *scope = analyzerCurrentScope(a);

    // Templates are analyzed per instance, when called
    if (decl->kind == DECL_FUNC && decl->func.template_param_count) return;

    for (uint32_t i = 0; i < arrLength(decl->attributes); ++i)
    {
        AstAttribute *attr = &decl->attributes.ptr[i];
        for (uint32_t j = 0; j < arrLength(attr->values); ++j)
        {
            analyzerAnalyzeExpr(a, attr->values.ptr[j], NULL);
        }
    }

    switch (decl->kind)
    {
    case DECL_FUNC: {
        decl->scope = NEW(compiler, Scope);
        scopeInit(compiler, decl->scope, scope, decl);

        analyzerAnalyzeExpr(a, decl->func.return_type, newBasicType(m, TYPE_TYPE));
        AstType *return_type = decl->func.return_type->as_type;

        if (!return_type)
        {
            ts__addErr(
                compiler, &decl->loc, "could not resolve return type for function");
            break;
        }

        bool param_types_valid = true;
        AstType **param_types = NULL;
        if (arrLength(decl->func.params) > 0)
        {
            param_types =
                NEW_ARRAY(compiler, AstType *, arrLength(decl->func.params));
        }

        analyzerPushScope(a, decl->scope);
        for (uint32_t i = 0; i < arrLength(decl->func.params); ++i)
        {
            AstDecl *param_decl = decl->func.params.ptr[i];
            TS_ASSERT(param_decl->kind == DECL_VAR);

            analyzerTryRegisterDecl(a, param_decl);
            analyzerAnalyzeDecl(a, param_decl);
            param_types[i] = param_decl->type;
            if (!param_types[i])
            {
                ts__addErr(
                    compiler,
                    &param_decl->loc,
                    "could not resolve type for function parameter");
                param_types_valid = false;
                continue;
            }

            if (param_decl->var.parameter_kind == PARAM_OUT)
            {
                // We pass a pointer when it's pass-by-reference
                param_types[i] =
                    newPointerType(m, SpvStorageClassFunction, param_types[i]);
            }
        }

        if (param_types_valid)
        {
            decl->type = newFuncType(
                m, return_type, param_types, arrLength(decl->func.params));
        }

        bool found_return = (return_type->kind == TYPE_VOID);

        for (uint32_t i = 0; i < arrLength(decl->func.stmts); ++i)
        {
            AstStmt *stmt = decl->func.stmts.ptr[i];
            analyzerAnalyzeStmt(a, stmt);

            if (stmtAlwaysReturns(stmt))
            {
                found_return = true;
            }
        }
        analyzerPopScope(a, decl->scope);

        if (!found_return)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "missing return statement for function '%s'", decl->name);
        }

        if (strcmp(m->entry_point, decl->name) == 0)
        {
            decl->func.called = true;
            m->entry_point_func = decl;

            switch (m->stage)
            {
            case TS_SHADER_STAGE_VERTEX: break;
            case TS_SHADER_STAGE_FRAGMENT: break;
            case TS_SHADER_STAGE_COMPUTE: {
                bool got_numthreads = false;

                for (uint32_t i = 0; i < arrLength(decl->attributes); ++i)
                {
                    AstAttribute *attr = &decl->attributes.ptr[i];

                    if (ts__strcasecmp(attr->name, "numthreads") == 0)
                    {
                        got_numthreads = true;
                        if (arrLength(attr->values) != 3)
                        {
                            ts__addErr(
                                compiler,
                                &decl->loc,
                                "numthreads attribute must have exactly 3 integer "
                                "parameters");
                        }
                        else
                        {
                            for (size_t j = 0; j < 3; ++j)
                            {
                                if (!attr->values.ptr[j]->resolved_int)
                                {
                                    ts__addErr(
                                        compiler,
                                        &attr->values.ptr[j]->loc,
                                        "could not resolve integer from expression");
                                }
                                else
                                {
                                    uint32_t dim =
                                        (uint32_t)*attr->values.ptr[j]->resolved_int;

                                    m->compute_dims[j] = dim;
                                }
                            }
                        }
                    }
                }

                if (!got_numthreads)
                {
                    ts__addErr(
                        compiler,
                        &decl->loc,
                        "thread group size [numthreads(x,y,z)] is missing from the "
                        "entry-point function");
                }

                break;
            }
            }

            analyzerRecursivelyCheckForSemanticStrings(a, decl);
        }

        break;
    }

    case DECL_VAR: {
        if (decl->var.storage_class == VAR_STORAGE_CLASS_FUNCTION && !a->scope_func)
        {
            ts__addErr(
                compiler, &decl->loc, "variable declaration must be inside a function");
            break;
        }

        analyzerAnalyzeExpr(a, decl->var.type_expr, newBasicType(m, TYPE_TYPE));
        if (!decl->var.type_expr->as_type)
        {
            break;
        }

        if (decl->var.value_expr)
        {
            analyzerAnalyzeExpr(a, decl->var.value_expr, decl->var.type_expr->as_type);

            if (decl->var.value_expr->type &&
                decl->var.type_expr->as_type->kind == TYPE_RUNTIME_ARRAY &&
                decl->var.value_expr->type->kind == TYPE_ARRAY &&
                decl->var.type_expr->as_type->array.sub == decl->var.value_expr->type->array.sub)
            {
                decl->var.type_expr->as_type = decl->var.value_expr->type;
            }
        }

        decl->type = decl->var.type_expr->as_type;

        if (decl->var.storage_class == VAR_STORAGE_CLASS_FUNCTION && a->scope_func)
        {
            arrPush(compiler, &a->scope_func->func.var_decls, decl);
        }

        AstType *struct_type = ts__getStructType(decl->type);

        if (struct_type)
        {
            decl->scope = NEW(compiler, Scope);
            scopeInit(compiler, decl->scope, NULL, decl);
            for (uint32_t i = 0; i < struct_type->struct_.field_count; ++i)
            {
                AstDecl *field_decl = struct_type->struct_.field_decls[i];
                scopeAdd(decl->scope, field_decl->name, field_decl);
            }
        }

        bool is_push_constant = false;
        bool got_binding_index = false;

        uint32_t binding_index = 0;
        uint32_t set_index = 0;

        // Retrieve set & binding from attributes
        for (uint32_t i = 0; i < arrLength(decl->attributes); ++i)
        {
            AstAttribute *attr = &decl->attributes.ptr[i];

            if (decl->var.storage_class == VAR_STORAGE_CLASS_UNIFORM &&
                strcmp(attr->name, "vk::push_constant") == 0)
            {
                decl->var.storage_class = VAR_STORAGE_CLASS_PUSH_CONSTANT;
                is_push_constant = true;
            }

            if (decl->var.storage_class == VAR_STORAGE_CLASS_UNIFORM &&
                strcmp(attr->name, "vk::binding") == 0)
            {
                if (attr->values.len >= 1)
                {
                    if (!attr->values.ptr[0]->resolved_int)
                    {
                        ts__addErr(
                            compiler,
                            &attr->values.ptr[0]->loc,
                            "could not resolve integer from expression");
                    }
                    else
                    {
                        binding_index = (uint32_t)*attr->values.ptr[0]->resolved_int;
                        got_binding_index = true;
                    }
                }

                if (attr->values.len >= 2)
                {
                    if (!attr->values.ptr[1]->resolved_int)
                    {
                        ts__addErr(
                            compiler,
                            &attr->values.ptr[1]->loc,
                            "could not resolve integer from expression");
                    }
                    else
                    {
                        set_index = (uint32_t)*attr->values.ptr[1]->resolved_int;
                    }
                }
            }
        }

        if (is_push_constant && got_binding_index)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "vk::push_constant attribute cannot be used together with vk::binding attribute");
            break;
        }

        if (is_push_constant && decl->type->kind != TYPE_STRUCT)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "push constants must be of a struct type");
            break;
        }

        if (decl->var.storage_class == VAR_STORAGE_CLASS_UNIFORM)
        {
            if (!got_binding_index)
            {
                binding_index = a->last_uniform_binding++;
            }
            else if (set_index == 0)
            {
                a->last_uniform_binding = binding_index + 1;
            }

            decl->var.binding = binding_index;
            decl->var.set = set_index;
        }

        break;
    }

    case DECL_TYPE_ALIAS:

    case DECL_STRUCT_FIELD: {
        analyzerAnalyzeExpr(a, decl->var.type_expr, newBasicType(m, TYPE_TYPE));
        if (!decl->var.type_expr->as_type)
        {
            ts__addErr(
                compiler,
                &decl->loc,
                "could not resolve type of struct field '%s'", decl->name);
        }

        decl->type = decl->var.type_expr->as_type;

        AstType *struct_type = ts__getStructType(decl->type);
        if (struct_type)
        {
            decl->scope = NEW(compiler, Scope);
            scopeInit(compiler, decl->scope, NULL, decl);
            for (uint32_t i = 0; i < struct_type->struct_.field_count; ++i)
            {
                AstDecl *field_decl = struct_type->struct_.field_decls[i];
                scopeAdd(decl->scope, field_decl->name, field_decl);
            }
        }

        break;
    }

    case DECL_STRUCT: {
        uint32_t field_count = arrLength(decl->struct_.fields);
        AstType **field_types = NEW_ARRAY(compiler, AstType *, field_count);

        decl->scope = NEW(compiler, Scope);
        scopeInit(compiler, decl->scope, scope, NULL);

        bool got_all_field_types = true;

        analyzerPushScope(a, decl->scope);
        for (uint32_t i = 0; i < arrLength(decl->struct_.fields); ++i)
        {
            AstDecl *field = decl->struct_.fields.ptr[i];
            field->var.field_index = i;
            analyzerTryRegisterDecl(a, field);
            analyzerAnalyzeDecl(a, field);

            field_types[i] = field->type;
            if (!field_types[i])
            {
                got_all_field_types = false;
            }
        }
        analyzerPopScope(a, decl->scope);

        if (!got_all_field_types)
        {
            ts__addErr(
                a->compiler,
                &decl->loc,
                "could not create a complete struct type for '%s'",
                decl->name);
            break;
        }

        decl->type = newBasicType(m, TYPE_TYPE);
        decl->as_type = newStructType(
            m, decl->name, field_types, decl->struct_.fields.ptr, field_count);

        for (uint32_t i = 0; i < arrLength(decl->struct_.fields); ++i)
        {
            if (field_types[i]->kind == TYPE_MATRIX)
            {
                IRMemberDecoration member_dec = {0};
                member_dec.kind = SpvDecorationRowMajor;
                member_dec.member_index = i;
                arrPush(compiler, &decl->as_type->struct_.field_decorations, member_dec);

                uint32_t row_stride =
                    field_types[i]->matrix.col_count *
                    typeSizeOf(m, field_types[i]->matrix.col_type->vector.elem_type);

                member_dec.kind = SpvDecorationMatrixStride;
                member_dec.member_index = i;
                member_dec.value = row_stride;
                arrPush(compiler, &decl->as_type->struct_.field_decorations, member_dec);
            }
        }

        break;
    }

    case DECL_ALIAS: {
        char *name = decl->name;

        AstDecl *accessed = decl->alias.accessed;
        TS_ASSERT(accessed->type);
        switch (accessed->type->kind)
        {
        case TYPE_CONSTANT_BUFFER:
        {
            TS_ASSERT(accessed->scope);
            AstDecl *field_decl = scopeGetLocal(accessed->scope, name);
            TS_ASSERT(field_decl->type);
            decl->type = field_decl->type;
            decl->scope = field_decl->scope;
            decl->alias.field_decl = field_decl;
            break;
        }

        default: break;
        }

        break;
    }
    }
}

void ts__analyze(
    TsCompiler *compiler, Module *module, AstDecl **decls, size_t decl_count)
{
    Analyzer *a = NEW(compiler, Analyzer);
    memset(a, 0, sizeof(*a));
    a->compiler = compiler;
    a->module = module;
    a->module->decls = decls;
    a->module->decl_count = decl_count;

    TS_ASSERT(!module->scope);

    module->scope = NEW(compiler, Scope);
    scopeInit(compiler, module->scope, NULL, NULL);

    analyzerPushScope(a, a->module->scope);

    for (uint32_t i = 0; i < decl_count; ++i)
    {
        AstDecl *decl = decls[i];
        analyzerTryRegisterDecl(a, decl);
    }

    // Template instances are emitted ahead of the declaration that needed them
    ArrayOfAstDeclPtr all_decls = {0};
    for (uint32_t i = 0; i < decl_count; ++i)
    {
        AstDecl *decl = decls[i];
        size_t before = a->template_instances.len;
        analyzerAnalyzeDecl(a, decl);
        for (size_t k = before; k < a->template_instances.len; ++k)
        {
            arrPush(compiler, &all_decls, a->template_instances.ptr[k]);
        }
        arrPush(compiler, &all_decls, decl);
    }
    a->module->decls = all_decls.ptr;
    a->module->decl_count = all_decls.len;

    analyzerPopScope(a, a->module->scope);
}
